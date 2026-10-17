"""验证看门狗配置失败时的恢复和设备占用保护，不操作实际硬件。"""

import importlib.util
from pathlib import Path
import subprocess

import pytest


spec = importlib.util.spec_from_file_location('install_watchdog', Path(__file__).parents[1] / 'install_watchdog.py')
watchdog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(watchdog)


@pytest.fixture
def prepared(tmp_path, monkeypatch):
    monkeypatch.setattr(watchdog, 'saved_switches', lambda: None)
    monkeypatch.setattr(watchdog, 'CONFIG', tmp_path / 'system.conf.d/watchdog.conf')
    monkeypatch.setattr(watchdog, 'PANIC_CONFIG', tmp_path / 'sysctl.d/recovery.conf')
    monkeypatch.setattr(watchdog, 'PANIC_TIMEOUT', tmp_path / 'panic')
    watchdog.PANIC_TIMEOUT.write_text('0\n')
    monkeypatch.setattr(watchdog.os, 'geteuid', lambda: 0)
    monkeypatch.setattr(watchdog, 'devices', lambda: [
        {'device': '/dev/watchdog0', 'driver': 'gpio-wdt'},
        {'device': '/dev/watchdog1', 'driver': 'dw_wdt'},
    ])
    monkeypatch.setattr(watchdog, 'owners', lambda device: [1] if device == '/dev/watchdog0' else [])
    monkeypatch.setattr(watchdog, 'manager_status', lambda: {
        'RuntimeWatchdogUSec': '1min', 'RebootWatchdogUSec': '1min',
    })
    monkeypatch.setattr(watchdog.time, 'sleep', lambda _: None)
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0)

    monkeypatch.setattr(watchdog.subprocess, 'run', run)
    return calls


@pytest.mark.parametrize('enabled', [False, True])
def test_upgrade_preserves_hardware_switch_without_online_reexec(prepared, monkeypatch, enabled):
    monkeypatch.setattr(watchdog, 'saved_switches', lambda: {'hardware_enabled': enabled, 'panic_enabled': False})
    result = watchdog.ensure()
    assert result['requested_enabled'] is enabled and not result['changed']
    assert not prepared and not watchdog.CONFIG.exists()


def test_upgrade_preserves_disabled_panic_switch(prepared, monkeypatch):
    monkeypatch.setattr(watchdog, 'saved_switches', lambda: {'hardware_enabled': False, 'panic_enabled': False})
    assert watchdog.ensure_panic_reboot()['enabled'] is False
    assert watchdog.PANIC_TIMEOUT.read_text().strip() == '0' and not watchdog.PANIC_CONFIG.exists()


def test_apply_reports_pid1_and_persists_runtime_and_shutdown_timeout(prepared):
    result = watchdog.apply('/dev/watchdog0', 60)
    assert result['owners'] == [1]
    assert result['RuntimeWatchdogUSec'] == '1min'
    content = watchdog.CONFIG.read_text()
    assert 'RuntimeWatchdogSec=60s' in content and 'RebootWatchdogSec=60s' in content
    assert 'WatchdogDevice=/dev/watchdog0' in content
    assert prepared == [['systemctl', 'daemon-reexec']]


@pytest.mark.parametrize('existing', [None, b'[Manager]\nRuntimeWatchdogSec=120s\n'])
def test_failed_activation_restores_previous_configuration(prepared, monkeypatch, existing):
    if existing is not None:
        watchdog.CONFIG.parent.mkdir(parents=True)
        watchdog.CONFIG.write_bytes(existing)
    monkeypatch.setattr(watchdog, 'manager_status', lambda: {'RuntimeWatchdogUSec': '0'})
    with pytest.raises(RuntimeError, match='未成功持有'):
        watchdog.apply('/dev/watchdog0', 60)
    if existing is None:
        assert not watchdog.CONFIG.exists()
    else:
        assert watchdog.CONFIG.read_bytes() == existing
    assert prepared == [['systemctl', 'daemon-reexec'], ['systemctl', 'daemon-reexec']]


def test_other_owner_is_not_displaced(prepared, monkeypatch):
    monkeypatch.setattr(watchdog, 'owners', lambda _: [123])
    with pytest.raises(RuntimeError, match='占用'):
        watchdog.apply('/dev/watchdog0', 60)
    assert not watchdog.CONFIG.exists() and prepared == []


def test_active_hardware_is_not_silently_switched(prepared, monkeypatch):
    monkeypatch.setattr(watchdog, 'owners', lambda device: [1] if device == '/dev/watchdog1' else [])
    with pytest.raises(RuntimeError, match='另一个看门狗'):
        watchdog.apply('/dev/watchdog0', 60)
    assert not watchdog.CONFIG.exists() and prepared == []


def test_software_watchdog_is_not_accepted(prepared, monkeypatch):
    monkeypatch.setattr(watchdog, 'devices', lambda: [{'device': '/dev/watchdog0', 'driver': 'softdog'}])
    with pytest.raises(RuntimeError, match='硬件看门狗'):
        watchdog.apply('/dev/watchdog0', 60)
    assert not watchdog.CONFIG.exists() and prepared == []


@pytest.mark.parametrize('device, timeout', [('/dev/watchdog0;reboot', 60), ('/dev/watchdog0', 0), ('/dev/watchdog0', True)])
def test_invalid_parameters_do_not_change_system_configuration(prepared, device, timeout):
    with pytest.raises(ValueError):
        watchdog.apply(device, timeout)
    assert not watchdog.CONFIG.exists() and prepared == []


def test_ensure_keeps_existing_watchdog_without_reexec(prepared):
    # Custom board configuration is never silently overwritten.
    result = watchdog.ensure()
    assert result['enabled'] and not result['changed']
    assert result['device'] == '/dev/watchdog0'
    assert prepared == []


def test_ensure_reports_missing_hardware_without_claiming_protection(prepared, monkeypatch):
    monkeypatch.setattr(watchdog, 'devices', lambda: [])
    assert watchdog.ensure()['enabled'] is False
    assert prepared == []


def test_ensure_selects_vendor_dw_driver_instead_of_device_number(prepared, monkeypatch):
    monkeypatch.setattr(watchdog, 'devices', lambda: [
        {'device': '/dev/watchdog0', 'driver': 'dw_wdt'},
        {'device': '/dev/watchdog1', 'driver': 'gpio-wdt'},
    ])
    monkeypatch.setattr(watchdog, 'owners', lambda device: [])
    selected = []
    monkeypatch.setattr(watchdog, 'apply', lambda device, seconds: selected.append((device, seconds)) or {})
    assert watchdog.ensure()['changed']
    assert selected == [('/dev/watchdog0', 60)]


def test_managed_gpio_is_migrated_only_on_next_boot(prepared):
    watchdog.write_config({'device': '/dev/watchdog0', 'driver': 'gpio-wdt'}, 60)
    result = watchdog.ensure()
    assert result['device'] == '/dev/watchdog0'
    assert result['configured_device'] == '/dev/watchdog1'
    assert result['reboot_required']
    assert 'WatchdogDevice=/dev/watchdog1' in watchdog.CONFIG.read_text()
    # In particular, no daemon-reexec can close the old live watchdog.
    assert not prepared
    assert not watchdog.ensure()['changed']


def test_occupied_dw_is_not_staged_over_managed_gpio(prepared, monkeypatch):
    watchdog.write_config({'device': '/dev/watchdog0', 'driver': 'gpio-wdt'}, 60)
    original = watchdog.CONFIG.read_bytes()
    monkeypatch.setattr(watchdog, 'owners', lambda d: [1] if d.endswith('0') else [456])
    with pytest.raises(RuntimeError, match='占用'):
        watchdog.ensure()
    assert watchdog.CONFIG.read_bytes() == original and not prepared


def test_active_dw_does_not_revert_to_gpio(prepared, monkeypatch):
    monkeypatch.setattr(watchdog, 'owners', lambda d: [1] if d.endswith('1') else [])
    result = watchdog.ensure()
    assert result['device'] == '/dev/watchdog1' and not result['changed']
    assert not prepared


def test_occupied_vendor_watchdog_does_not_silently_fall_back_to_gpio(prepared, monkeypatch):
    monkeypatch.setattr(watchdog, 'owners', lambda d: [456] if d.endswith('1') else [])
    with pytest.raises(RuntimeError, match='未回退'):
        watchdog.ensure()
    assert not prepared and not watchdog.CONFIG.exists()


@pytest.mark.parametrize('original,expected', [(0, 10), (5, 5), (-1, -1)])
def test_panic_recovery_enables_reboot_and_preserves_nonzero_admin_timeout(prepared, original, expected):
    watchdog.PANIC_TIMEOUT.write_text(str(original))
    result = watchdog.ensure_panic_reboot()
    assert result['timeout_sec'] == expected
    assert int(watchdog.PANIC_TIMEOUT.read_text()) == expected
    assert f'kernel.panic={expected}\n' in watchdog.PANIC_CONFIG.read_text()
    assert not prepared


def test_panic_recovery_failure_rolls_back_persistent_configuration(prepared, monkeypatch):
    watchdog.PANIC_CONFIG.parent.mkdir(parents=True)
    watchdog.PANIC_CONFIG.write_text('# administrator config\n')
    original = Path.write_text
    def fail_proc(path, *args, **kwargs):
        if path == watchdog.PANIC_TIMEOUT:
            raise PermissionError('proc write rejected')
        return original(path, *args, **kwargs)
    monkeypatch.setattr(Path, 'write_text', fail_proc)
    with pytest.raises(PermissionError):
        watchdog.ensure_panic_reboot()
    assert watchdog.PANIC_CONFIG.read_text() == '# administrator config\n'
    assert watchdog.PANIC_TIMEOUT.read_text() == '0\n'


def test_ensure_does_not_take_over_occupied_hardware(prepared, monkeypatch):
    monkeypatch.setattr(watchdog, 'owners', lambda device: [123])
    with pytest.raises(RuntimeError, match='占用'):
        watchdog.ensure()
    assert prepared == []


def test_monitor_installation_is_independent_and_persistent(prepared, tmp_path, monkeypatch):
    monkeypatch.setattr(watchdog, 'MONITOR_DIR', tmp_path / 'lib/recovery')
    monkeypatch.setattr(watchdog, 'SYSTEMD_DIR', tmp_path / 'systemd')
    watchdog.install_monitor()
    assert (watchdog.MONITOR_DIR / 'recovery_monitor.py').exists()
    service = (watchdog.SYSTEMD_DIR / 'rk3588-recovery.service').read_text()
    timer = (watchdog.SYSTEMD_DIR / 'rk3588-recovery.timer').read_text()
    assert 'Type=oneshot' in service and 'TimeoutStartSec=25' in service
    assert 'OnUnitActiveSec=30' in timer and 'WantedBy=timers.target' in timer
    assert prepared == [['systemctl', 'daemon-reload'],
                        ['systemctl', 'enable', '--now', 'rk3588-recovery.timer']]
