"""Vendor-board states are reproduced without opening a watchdog or changing hardware."""
import gzip
from pathlib import Path
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from rk3588_runtime import management


@pytest.fixture
def board(tmp_path, monkeypatch):
    paths = {
        'WATCHDOG_SYSFS': 'sysfs', 'WATCHDOG_DEVICES': 'dev', 'WATCHDOG_FDS': 'fds',
        'DEVICE_TREE': 'dt', 'KERNEL_CONFIG': 'config.gz', 'BOOT_KERNEL_CONFIG': 'boot-config',
        'PLATFORM_DRIVERS': 'drivers', 'WATCHDOG_CONFIG': 'watchdog.conf',
        'PANIC_TIMEOUT_FILE': 'panic', 'WATCHDOG_VALIDATION_FILE': 'assessment.json',
        'MACHINE_ID_FILE': 'machine-id',
    }
    for key, name in paths.items():
        monkeypatch.setattr(management, key, tmp_path / name)
    for key in ('WATCHDOG_SYSFS', 'WATCHDOG_DEVICES', 'WATCHDOG_FDS', 'DEVICE_TREE', 'PLATFORM_DRIVERS'):
        getattr(management, key).mkdir()
    management.PANIC_TIMEOUT_FILE.write_text('10\n')
    calls = []

    def run(command, **kwargs):
        calls.append(command)
        assert command[:2] == ['systemctl', 'show']
        return subprocess.CompletedProcess(command, 0,
                                           'RuntimeWatchdogUSec=1min 29s\nRebootWatchdogUSec=1min\n', '')

    monkeypatch.setattr(management.subprocess, 'run', run)
    original_open = Path.open

    def guarded_open(path, *args, **kwargs):
        assert not path.is_relative_to(management.WATCHDOG_DEVICES), 'must never open watchdog nodes'
        return original_open(path, *args, **kwargs)

    monkeypatch.setattr(Path, 'open', guarded_open)

    class Board:
        def config(self, text):
            management.KERNEL_CONFIG.write_bytes(gzip.compress(text.encode()))

        def node(self, status='disabled', name='watchdog@feaf0000'):
            node = management.DEVICE_TREE / name
            node.mkdir(parents=True)
            (node / 'compatible').write_bytes(b'snps,dw-wdt\0')
            if status is not None:
                (node / 'status').write_bytes(status.encode() + b'\0')
            return node

        def device(self, driver='dw_wdt', number=7, owned=False, node_present=True, legacy=False):
            name = f'watchdog{number}'
            entry = management.WATCHDOG_SYSFS / name / 'device'
            entry.mkdir(parents=True)
            driver_path = management.PLATFORM_DRIVERS / driver
            driver_path.mkdir(exist_ok=True)
            (entry / 'driver').symlink_to(driver_path)
            # stat() sees a character device; opening it is forbidden by guarded_open.
            if node_present:
                (management.WATCHDOG_DEVICES / name).symlink_to('/dev/null')
            if owned:
                target = management.WATCHDOG_DEVICES / ('watchdog' if legacy else name)
                (management.WATCHDOG_FDS / '9').symlink_to(target)

    yield Board()
    assert len(calls) <= 2


def test_reported_atk_board_has_driver_but_disabled_device_tree(board):
    board.config('CONFIG_WATCHDOG=y\nCONFIG_DW_WATCHDOG=y\n# CONFIG_GPIO_WATCHDOG is not set\n# CONFIG_WATCHDOG_SYSFS is not set\n')
    board.node()
    (management.PLATFORM_DRIVERS / 'dw_wdt').mkdir()
    status = management.hardware_status()
    assert not status['enabled'] and status['state'] == 'device_tree_disabled'
    assert status['label'] == '设备树未开启'
    assert status['diagnostics']['kernel_config']['DW_WATCHDOG'] == 'y'
    assert status['diagnostics']['kernel_config']['WATCHDOG_SYSFS'] == 'n'
    assert status['panic_timeout_sec'] == 10
    assert any('厂商' in step and '设备树' in step for step in status['next_steps'])


@pytest.mark.parametrize('number,legacy', [(0, False), (7, False), (0, True)])
def test_actual_driver_and_ownership_work_with_different_numbering_and_legacy_alias(board, number, legacy):
    board.device(number=number, owned=True, legacy=legacy)
    status = management.hardware_status()
    assert status['enabled'] and status['state'] == 'active'
    assert status['device'].endswith(f'watchdog{number}') and status['driver'] == 'dw_wdt'
    assert status['verification_state'] == 'unverified'


def test_disabled_parent_blocks_enabled_watchdog_child(board):
    board.node('okay', 'bus/watchdog@feaf0000')
    (management.DEVICE_TREE / 'bus/status').write_bytes(b'disabled\0')
    status = management.hardware_status()
    assert status['state'] == 'device_tree_disabled'
    assert status['diagnostics']['device_tree'][0]['disabled_by'] == 'bus'


@pytest.mark.parametrize('status', ['okay', 'ok', None])
def test_enabled_tree_without_registered_device_does_not_claim_protection(board, status):
    board.node(status)
    board.config('CONFIG_WATCHDOG=y\nCONFIG_DW_WATCHDOG=y\n')
    result = management.hardware_status()
    assert not result['enabled'] and result['state'] == 'device_unavailable'


@pytest.mark.parametrize('config', ['# CONFIG_WATCHDOG is not set\n',
                                  'CONFIG_WATCHDOG=y\n# CONFIG_DW_WATCHDOG is not set\n# CONFIG_GPIO_WATCHDOG is not set\n'])
def test_kernel_without_hardware_driver_reports_required_kernel_support(board, config):
    board.config(config)
    assert management.hardware_status()['state'] == 'driver_missing'


def test_module_not_loaded_is_distinct_from_missing_driver(board):
    board.node('okay')
    board.config('CONFIG_WATCHDOG=y\nCONFIG_DW_WATCHDOG=m\n')
    assert management.hardware_status()['state'] == 'driver_not_loaded'


def test_missing_config_and_missing_devices_do_not_assert_hardware_is_absent(board):
    result = management.hardware_status()
    assert result['state'] == 'device_unavailable'
    assert '无法判断板卡是否具备' in result['summary']


def test_boot_config_is_used_when_proc_config_is_unavailable(board):
    management.BOOT_KERNEL_CONFIG.write_text('# CONFIG_WATCHDOG is not set\n')
    assert management.hardware_status()['state'] == 'driver_missing'


def test_registered_hardware_without_device_node_is_distinct(board):
    board.device(node_present=False)
    assert management.hardware_status()['state'] == 'device_node_missing'


def test_systemd_setting_alone_cannot_claim_active_hardware(board):
    board.device()
    result = management.hardware_status()
    assert result['state'] == 'not_managed' and not result['enabled']


def test_ownership_with_zero_runtime_timeout_does_not_claim_running_protection(board, monkeypatch):
    board.device(owned=True)
    monkeypatch.setattr(management.subprocess, 'run', lambda *a, **k: subprocess.CompletedProcess(a, 0, 'RuntimeWatchdogUSec=0\n', ''))
    assert management.hardware_status()['state'] == 'not_enabled'


@pytest.mark.parametrize('driver,state', [('softdog', 'software_only'), ('vendor-custom-wdt', 'unsupported_driver')])
def test_software_and_unknown_drivers_are_not_misreported_as_hardware_protection(board, driver, state):
    board.device(driver=driver, owned=True)
    result = management.hardware_status()
    assert result['state'] == state and not result['enabled']


def test_systemd_failure_preserves_independent_panic_status(board, monkeypatch):
    board.device(owned=True)
    monkeypatch.setattr(management.subprocess, 'run', lambda *a, **k: subprocess.CompletedProcess(a, 1, '', 'permission denied'))
    result = management.hardware_status()
    assert not result['enabled'] and result['state'] == 'inspection_failed'
    assert result['panic_timeout_sec'] == 10 and 'permission denied' in result['error']


def test_permission_failure_is_unknown_instead_of_disabled_or_unavailable(board, monkeypatch):
    original = Path.iterdir

    def denied(path):
        if path == management.WATCHDOG_FDS:
            raise PermissionError('denied')
        return original(path)

    monkeypatch.setattr(Path, 'iterdir', denied)
    result = management.hardware_status()
    assert result['state'] == 'inspection_failed' and result['diagnostics']['errors']


def test_network_dependencies_are_reported_without_running_missing_tools(board, monkeypatch):
    from rk3588_runtime import network_policy, network_recovery
    monkeypatch.setattr(management, 'unit_status', lambda _: {'installed': True, 'active': False, 'enabled': False})
    monkeypatch.setattr(management.manager, 'read_json', lambda *a: {})
    monkeypatch.setattr(network_policy, 'read', lambda: network_policy.validate(network_policy.DEFAULTS))
    monkeypatch.setattr(management.shutil, 'which', lambda _: None)
    monkeypatch.setattr(network_recovery, 'interfaces', lambda: pytest.fail('must not execute missing nmcli'))
    result = management.network_snapshot()
    assert result['requirements']['ready'] is False
    assert result['requirements']['missing_tools'] == ['nmcli', 'ip', 'ping']
    assert result['interfaces'] == [] and result['interface_error'] is None


def test_network_cannot_enable_with_missing_tools_or_overwrite_policy(board, monkeypatch):
    from rk3588_runtime import network_policy
    monkeypatch.setattr(management, 'unit_status', lambda _: {'installed': True})
    monkeypatch.setattr(management.shutil, 'which', lambda _: None)
    monkeypatch.setattr(network_policy, 'write', lambda *a: pytest.fail('policy must not be changed'))
    settings = network_policy.validate({**network_policy.DEFAULTS, 'enabled': True,
                                        'targets': [{'host': '192.0.2.10', 'method': 'ping'}]})
    with pytest.raises(RuntimeError, match='启用网络恢复需要'):
        management.save_network_policy(settings, None)
