"""System switches use temporary files; no real sysctl, watchdog, or reboot is touched."""
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
from pathlib import Path
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from rk3588_runtime import system_recovery as system
from rk3588_runtime.recovery_policy import PolicyConflict


@pytest.fixture
def board(tmp_path, monkeypatch):
    for key, name in [('POLICY_FILE', 'settings.json'), ('WATCHDOG_OVERRIDE', 'watchdog.conf'),
                      ('PANIC_OVERRIDE', 'panic.conf'), ('PANIC_FILE', 'panic'), ('BOOT_ID_FILE', 'boot-id')]:
        monkeypatch.setattr(system, key, tmp_path / name)
    system.PANIC_FILE.write_text('10\n')
    system.BOOT_ID_FILE.write_text('boot-a\n')
    monkeypatch.setattr(subprocess, 'run', lambda *a, **k: pytest.fail('saving must not execute system commands'))
    hardware = {'enabled': True, 'device': '/dev/watchdog7', 'configured_device': '/dev/watchdog7',
                'driver': 'dw_wdt', 'runtime_timeout': '1min 29s', 'reboot_timeout': '1min',
                'panic_timeout_sec': 10, 'error': None, 'reboot_required': False,
                'diagnostics': {'devices': [{'device': '/dev/watchdog7', 'driver': 'dw_wdt', 'node_present': True}]}}

    def reader():
        return {**deepcopy(hardware), 'panic_timeout_sec': int(system.PANIC_FILE.read_text())}

    class Board:
        state = hardware
        read = staticmethod(reader)

        def snapshot(self):
            return system.snapshot(reader())

        def save(self, hardware_enabled=True, panic_enabled=True, **timeouts):
            policy = {**(self.snapshot()['policy'] or {}), 'hardware_enabled': hardware_enabled,
                      'panic_enabled': panic_enabled, **timeouts}
            system.save(policy,
                        self.snapshot()['revision'], reader)
            return self.snapshot()

    return Board()


def test_initial_read_follows_actual_state_without_creating_settings(board):
    value = board.snapshot()
    assert value['policy'] == {'hardware_enabled': True, 'panic_enabled': True,
                               'watchdog_timeout_sec': 89, 'reboot_timeout_sec': 60, 'panic_timeout_sec': 10}
    assert value['can_enable_hardware'] and not system.POLICY_FILE.exists()
    assert not system.POLICY_FILE.with_suffix('.lock').exists()


def test_hardware_off_is_persisted_but_running_protection_is_not_stopped(board):
    result = board.save(False, True)
    assert 'RuntimeWatchdogSec=0s' in system.WATCHDOG_OVERRIDE.read_text()
    assert 'RebootWatchdogSec=0s' in system.WATCHDOG_OVERRIDE.read_text()
    assert board.read()['enabled'] is True
    assert result['hardware_pending'] and result['hardware_apply_error'] is None
    assert system.PANIC_FILE.read_text().strip() == '10'


def test_panic_off_is_immediate_and_does_not_change_hardware_configuration(board):
    result = board.save(True, False)
    assert system.PANIC_FILE.read_text().strip() == '0'
    assert 'kernel.panic=0' in system.PANIC_OVERRIDE.read_text()
    assert not system.WATCHDOG_OVERRIDE.exists()
    assert result['panic_in_sync'] and not result['hardware_pending']


def test_later_panic_changes_preserve_pending_hardware_off(board):
    board.save(False, True)
    original = system.WATCHDOG_OVERRIDE.read_bytes()
    result = board.save(False, False)
    assert result['hardware_pending'] and not result['policy']['hardware_enabled']
    assert system.WATCHDOG_OVERRIDE.read_bytes() == original


def test_turning_hardware_back_on_before_reboot_cancels_pending_off(board):
    board.save(False, True)
    result = board.save(True, True)
    assert not result['hardware_pending']
    assert 'WatchdogDevice=/dev/watchdog7' in system.WATCHDOG_OVERRIDE.read_text()
    assert 'RuntimeWatchdogSec=89s' in system.WATCHDOG_OVERRIDE.read_text()


def test_new_hardware_enable_waits_for_boot_and_uses_actual_driver_number(board):
    board.state.update(enabled=False, device=None)
    result = board.save(True, True)
    assert result['hardware_pending']
    assert 'WatchdogDevice=/dev/watchdog7' in system.WATCHDOG_OVERRIDE.read_text()
    system.BOOT_ID_FILE.write_text('boot-b\n')
    board.state.update(enabled=True, device='/dev/watchdog7')
    result = board.snapshot()
    assert not result['hardware_pending'] and result['hardware_apply_error'] is None


def test_failed_application_after_boot_is_an_error_not_an_endless_pending_state(board):
    board.save(False, True)
    system.BOOT_ID_FILE.write_text('boot-b\n')
    result = board.snapshot()
    assert not result['hardware_pending'] and result['hardware_apply_error']
    board.state['enabled'] = False
    assert board.snapshot()['hardware_apply_error'] is None


def test_missing_hardware_blocks_enabling_but_allows_panic_switch(board):
    board.state.update(enabled=False, device=None, configured_device=None,
                       summary='设备树未开启', diagnostics={'devices': []})
    assert not board.snapshot()['can_enable_hardware']
    with pytest.raises(ValueError, match='设备树未开启'):
        board.save(True, True)
    assert not system.POLICY_FILE.exists()
    result = board.save(False, False)
    assert result['panic_in_sync'] and not result['hardware_pending']


@pytest.mark.parametrize('original', [-1, 37])
def test_panic_reenable_preserves_previous_nonzero_timeout(board, original):
    system.PANIC_FILE.write_text(str(original))
    board.save(True, False)
    board.save(True, True)
    assert int(system.PANIC_FILE.read_text()) == original


@pytest.mark.parametrize('value', [None, {}, {'hardware_enabled': 1, 'panic_enabled': True},
                                 {'hardware_enabled': True, 'panic_enabled': True, 'app_restart_enabled': False}])
def test_unknown_fields_and_non_boolean_values_do_not_modify_settings(board, value):
    with pytest.raises(ValueError):
        system.save(value, 'revision', board.read)
    assert not system.POLICY_FILE.exists()


def test_stale_revision_does_not_overwrite_a_newer_choice(board):
    revision = board.snapshot()['revision']
    board.save(False, True)
    with pytest.raises(PolicyConflict):
        system.save({'hardware_enabled': True, 'panic_enabled': False}, revision, board.read)
    assert system.read()['hardware_enabled'] is False


def test_actual_panic_change_also_invalidates_revision(board):
    revision = board.snapshot()['revision']
    system.PANIC_FILE.write_text('20')
    with pytest.raises(PolicyConflict):
        system.save({'hardware_enabled': False, 'panic_enabled': False}, revision, board.read)
    assert not system.POLICY_FILE.exists()


def test_two_concurrent_saves_cannot_both_use_the_same_revision(board):
    revision = board.snapshot()['revision']

    def save():
        try:
            system.save({'hardware_enabled': False, 'panic_enabled': False}, revision, board.read)
            return 'saved'
        except PolicyConflict:
            return 'conflict'

    with ThreadPoolExecutor(max_workers=2) as pool:
        assert sorted(pool.map(lambda _: save(), range(2))) == ['conflict', 'saved']


def test_kernel_write_failure_restores_files_and_original_panic_state(board, monkeypatch):
    original = Path.write_text

    def reject(path, value, *args, **kwargs):
        if path == system.PANIC_FILE and value.strip() == '0':
            raise PermissionError('kernel setting is read-only')
        return original(path, value, *args, **kwargs)

    monkeypatch.setattr(Path, 'write_text', reject)
    with pytest.raises(RuntimeError, match='已恢复原设置'):
        board.save(False, False)
    assert not system.POLICY_FILE.exists() and not system.WATCHDOG_OVERRIDE.exists()
    assert not system.PANIC_OVERRIDE.exists() and system.PANIC_FILE.read_text().strip() == '10'


def test_file_write_failure_rolls_back_previously_saved_choices(board, monkeypatch):
    board.save(False, True)
    old = {p: p.read_bytes() for p in (system.POLICY_FILE, system.WATCHDOG_OVERRIDE, system.PANIC_OVERRIDE)}
    original = system._atomic
    failed = False

    def reject(path, content):
        nonlocal failed
        if path == system.POLICY_FILE and not failed:
            failed = True
            raise OSError('disk full')
        return original(path, content)

    monkeypatch.setattr(system, '_atomic', reject)
    with pytest.raises(RuntimeError, match='已恢复原设置'):
        board.save(True, False)
    assert all(p.read_bytes() == content for p, content in old.items())
    assert system.PANIC_FILE.read_text().strip() == '10'


def test_custom_override_is_not_overwritten(board):
    system.WATCHDOG_OVERRIDE.write_text('# custom administrator configuration\n')
    with pytest.raises(ValueError, match='自定义配置'):
        board.save(False, False)
    assert not system.POLICY_FILE.exists() and system.PANIC_FILE.read_text().strip() == '10'


def test_invalid_saved_policy_is_reported_and_not_silently_replaced(board):
    system.POLICY_FILE.write_text('{broken')
    assert board.snapshot()['error']
    with pytest.raises(ValueError):
        board.save(False, False)
    assert system.POLICY_FILE.read_text() == '{broken'


def test_external_hardware_drift_is_not_misreported_as_a_reboot_that_never_happened(board):
    board.save(True, False)
    board.state['enabled'] = False
    result = board.snapshot()
    assert result['hardware_apply_error'] and '已重启' not in result['hardware_apply_error']


def test_legacy_watchdog_alias_does_not_create_false_pending_switch(board):
    board.state.update(device='/dev/watchdog0', configured_device='/dev/watchdog')
    board.save(True, True)
    assert not board.snapshot()['hardware_pending'] and board.snapshot()['hardware_apply_error'] is None


def test_panic_save_does_not_disable_an_unrelated_software_watchdog(board):
    board.state.update(enabled=False, device=None, configured_device=None,
                       diagnostics={'devices': [{'device': '/dev/watchdog0', 'driver': 'softdog',
                                                  'node_present': True, 'manager_owned': True}]})
    board.save(False, False)
    assert not system.WATCHDOG_OVERRIDE.exists()
    assert system.PANIC_FILE.read_text().strip() == '0'


def test_timeout_changes_are_staged_while_panic_delay_is_applied_immediately(board):
    result = board.save(watchdog_timeout_sec=120, reboot_timeout_sec=180, panic_timeout_sec=25)
    assert result['policy']['watchdog_timeout_sec'] == 120
    assert result['policy']['reboot_timeout_sec'] == 180
    assert result['policy']['panic_timeout_sec'] == 25
    assert 'RuntimeWatchdogSec=120s' in system.WATCHDOG_OVERRIDE.read_text()
    assert 'RebootWatchdogSec=180s' in system.WATCHDOG_OVERRIDE.read_text()
    assert 'kernel.panic=25' in system.PANIC_OVERRIDE.read_text()
    assert system.PANIC_FILE.read_text().strip() == '25'
    assert result['hardware_pending'] and result['panic_in_sync']
    assert board.read()['runtime_timeout'] == '1min 29s'
    original = system.WATCHDOG_OVERRIDE.read_bytes()
    result = board.save(panic_timeout_sec=30)
    assert result['hardware_pending']
    assert system.WATCHDOG_OVERRIDE.read_bytes() == original


def test_timeout_pending_clears_after_boot_without_requiring_exact_hardware_rounding(board):
    board.save(watchdog_timeout_sec=60, reboot_timeout_sec=90)
    system.BOOT_ID_FILE.write_text('boot-b\n')
    board.state.update(runtime_timeout='1min 29s', reboot_timeout='2min 58s')
    result = board.snapshot()
    assert not result['hardware_pending'] and result['hardware_apply_error'] is None
    assert result['policy']['watchdog_timeout_sec'] == 60
    assert result['policy']['reboot_timeout_sec'] == 90


def test_panic_delay_mismatch_is_detected_and_can_be_reapplied(board):
    board.save(panic_timeout_sec=20)
    system.PANIC_FILE.write_text('30')
    assert not board.snapshot()['panic_in_sync']
    result = board.save()
    assert result['panic_in_sync'] and system.PANIC_FILE.read_text().strip() == '20'


@pytest.mark.parametrize('key,value', [
    ('watchdog_timeout_sec', 0), ('watchdog_timeout_sec', 86401), ('watchdog_timeout_sec', True),
    ('reboot_timeout_sec', -1), ('reboot_timeout_sec', 1.5), ('reboot_timeout_sec', '60'),
    ('panic_timeout_sec', 0), ('panic_timeout_sec', False), ('panic_timeout_sec', None),
    ('panic_timeout_sec', 2147483648), ('panic_timeout_sec', -2147483649),
])
def test_invalid_timeout_values_never_write_settings(board, key, value):
    with pytest.raises(ValueError):
        board.save(**{key: value})
    assert not system.POLICY_FILE.exists() and not system.WATCHDOG_OVERRIDE.exists()
    assert not system.PANIC_OVERRIDE.exists() and system.PANIC_FILE.read_text().strip() == '10'


def test_switch_only_legacy_requests_preserve_saved_timeouts(board):
    board.save(watchdog_timeout_sec=100, reboot_timeout_sec=200, panic_timeout_sec=37)
    result = board.snapshot()
    system.save({'hardware_enabled': True, 'panic_enabled': False}, result['revision'], board.read)
    assert board.snapshot()['hardware_pending']
    assert system.read()['watchdog_timeout_sec'] == 100 and system.read()['reboot_timeout_sec'] == 200
    board.save(panic_enabled=True)
    assert system.PANIC_FILE.read_text().strip() == '37'


def test_hardware_timeout_drift_invalidates_a_draft_revision(board):
    original = board.snapshot()
    board.state['runtime_timeout'] = '2min'
    with pytest.raises(PolicyConflict):
        system.save(original['policy'], original['revision'], board.read)
    assert not system.POLICY_FILE.exists()


def test_timeout_write_failure_restores_previous_timeouts_and_kernel_state(board, monkeypatch):
    board.save(watchdog_timeout_sec=120, panic_timeout_sec=20)
    paths = (system.POLICY_FILE, system.WATCHDOG_OVERRIDE, system.PANIC_OVERRIDE)
    original_files = {path: path.read_bytes() for path in paths}
    original = Path.write_text

    def reject(path, value, *args, **kwargs):
        if path == system.PANIC_FILE and value.strip() == '25':
            raise PermissionError('kernel setting is read-only')
        return original(path, value, *args, **kwargs)

    monkeypatch.setattr(Path, 'write_text', reject)
    with pytest.raises(RuntimeError, match='已恢复原设置'):
        board.save(watchdog_timeout_sec=180, reboot_timeout_sec=240, panic_timeout_sec=25)
    assert all(path.read_bytes() == content for path, content in original_files.items())
    assert system.PANIC_FILE.read_text().strip() == '20'
