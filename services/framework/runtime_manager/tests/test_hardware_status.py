"""Hardware ownership and a successful boot must not imply reset validation."""
import json
from pathlib import Path
import platform
import subprocess
import sys

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from rk3588_runtime import management


@pytest.fixture
def hardware(tmp_path, monkeypatch):
    monkeypatch.setattr(management.system_recovery, 'WATCHDOG_OVERRIDE', tmp_path / 'switches.conf')
    for attribute, name in [('WATCHDOG_CONFIG', 'watchdog.conf'), ('WATCHDOG_FDS', 'fds'),
                            ('WATCHDOG_SYSFS', 'sysfs'), ('PANIC_TIMEOUT_FILE', 'panic'),
                            ('WATCHDOG_VALIDATION_FILE', 'assessment.json'), ('MACHINE_ID_FILE', 'machine-id')]:
        monkeypatch.setattr(management, attribute, tmp_path / name)
    management.WATCHDOG_FDS.mkdir()
    (management.WATCHDOG_FDS / '9').symlink_to('/dev/watchdog0')
    device = management.WATCHDOG_SYSFS / 'watchdog0/device'
    device.mkdir(parents=True)
    (device / 'driver').symlink_to(tmp_path / 'drivers/gpio-wdt')
    management.WATCHDOG_CONFIG.write_text('[Manager]\nWatchdogDevice=/dev/watchdog1\n')
    management.PANIC_TIMEOUT_FILE.write_text('10\n')
    management.MACHINE_ID_FILE.write_text('test-board\n')
    monkeypatch.setattr(management.subprocess, 'run', lambda *a, **kw:
                        subprocess.CompletedProcess(a, 0, 'RuntimeWatchdogUSec=1min\nRebootWatchdogUSec=1min\n', ''))
    return tmp_path


def test_live_ownership_is_not_a_pass_and_pending_device_switch_is_visible(hardware):
    status = management.hardware_status()
    assert status['enabled'] and status['driver'] == 'gpio-wdt'
    assert status['verification_state'] == 'unverified'
    assert status['configured_device'] == '/dev/watchdog1' and status['reboot_required']
    assert status['panic_timeout_sec'] == 10


def test_reported_manual_power_cycle_is_failure_not_automatic_recovery(hardware):
    management.WATCHDOG_VALIDATION_FILE.write_text(json.dumps({
        'machine_id': 'test-board', 'kernel': platform.release(), 'driver': 'gpio-wdt',
        'result': 'failed', 'message': '等待约5分钟未恢复，人工断电重启',
    }))
    status = management.hardware_status()
    assert status['enabled'] and status['verification_state'] == 'failed'
    assert '人工断电' in status['verification_message']
    # That failure does not incorrectly certify or reject a different driver.
    (management.WATCHDOG_SYSFS / 'watchdog0/device/driver').unlink()
    (management.WATCHDOG_SYSFS / 'watchdog0/device/driver').symlink_to(hardware / 'drivers/dw_wdt')
    assert management.hardware_status()['verification_state'] == 'unverified'


@pytest.mark.parametrize('field,value', [('kernel', 'old-kernel'), ('machine_id', 'other-board')])
def test_old_acceptance_cannot_certify_changed_board_or_kernel(hardware, field, value):
    assessment = {'machine_id': 'test-board', 'kernel': platform.release(),
                  'driver': 'gpio-wdt', 'result': 'passed'}
    assessment[field] = value
    management.WATCHDOG_VALIDATION_FILE.write_text(json.dumps(assessment))
    assert management.hardware_status()['verification_state'] == 'unverified'


def test_boot_change_alone_cannot_certify_watchdog(hardware):
    (hardware / 'after.json').write_text(json.dumps({'reboot_observed': True, 'ssh_local_banner': 'SSH-2.0'}))
    assert management.hardware_status()['verification_state'] == 'unverified'


def test_switch_override_is_effective_even_without_legacy_installer_config(hardware):
    management.WATCHDOG_CONFIG.unlink()
    management.system_recovery.WATCHDOG_OVERRIDE.write_text('[Manager]\nWatchdogDevice=/dev/watchdog0\nRuntimeWatchdogSec=60s\n')
    status = management.hardware_status()
    assert status['configured_device'] == '/dev/watchdog0' and not status['reboot_required']


def test_pending_hardware_off_does_not_report_obsolete_device_migration(hardware):
    management.system_recovery.WATCHDOG_OVERRIDE.write_text('[Manager]\nRuntimeWatchdogSec=0s\n')
    status = management.hardware_status()
    assert status['enabled'] and status['configured_device'] is None and not status['reboot_required']
