import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

LIB = Path(__file__).resolve().parents[1] / 'python'
sys.path.insert(0, str(LIB))
from rk3588_runtime import manager, process_manager as pm, recovery_policy as policy, runtime_state


@pytest.fixture
def isolated(tmp_path, monkeypatch):
    monkeypatch.setattr(policy, 'POLICY_FILE', tmp_path / 'etc/recovery.json')
    monkeypatch.setattr(manager, 'STATUS_FILE', tmp_path / 'run/status.json')
    monkeypatch.setattr(manager, 'EVENTS_FILE', tmp_path / 'var/events.json')
    monkeypatch.setattr(runtime_state, 'STATE_FILE', tmp_path / 'apps/runtime.json')
    monkeypatch.setattr(runtime_state, 'LOCK_FILE', tmp_path / 'apps/runtime.lock')
    monkeypatch.setattr(pm, 'APPS_ROOT', tmp_path / 'apps')
    monkeypatch.setattr(pm, '_start_lock_path', tmp_path / 'apps/start.lock')
    monkeypatch.setattr(pm, 'recover_processes', lambda: None)
    monkeypatch.setattr(pm, '_current_boot_id', lambda: 'test-boot')
    monkeypatch.setattr(manager.background_services, 'sync_services_for_running_app', lambda: {'updated': [], 'errors': []})
    current = {'value': None}
    calls = []
    def start(app, config):
        calls.append((app, config))
        current['value'] = {'app': app, 'config': config, 'pid': 123}
        return 123
    monkeypatch.setattr(pm, 'start_app', start)
    monkeypatch.setattr(pm, 'get_running_app_context', lambda: current['value'])
    monkeypatch.setattr(pm, 'get_status', lambda app: {'status': 'running', 'pid': 123, 'config': 'production.json'})
    return calls, current


def test_restores_without_any_web_lifecycle_and_honors_manual_stop(isolated):
    calls, current = isolated
    runtime_state.set_vision_autostart('demo', True)
    runtime_state.mark_vision_started('demo', 'production.json')
    agent = manager.Manager()
    assert agent.tick()['current']['app'] == 'demo'
    assert calls == [('demo', 'production.json')]
    current['value'] = None
    runtime_state.mark_vision_stopped('demo')
    agent.tick()
    assert len(calls) == 1


def test_autostart_and_running_intent_both_required(isolated):
    calls, _ = isolated
    runtime_state.mark_vision_started('demo', 'production.json')
    manager.Manager().tick()
    assert not calls
    runtime_state.mark_vision_stopped('demo')
    runtime_state.set_vision_autostart('demo', True)
    manager.Manager().tick()
    assert not calls


def test_disabled_policy_is_loaded_by_running_agent(isolated):
    calls, _ = isolated
    runtime_state.set_vision_autostart('demo', True)
    runtime_state.mark_vision_started('demo', 'production.json')
    agent = manager.Manager()
    policy.write({**policy.DEFAULTS, 'manager_enabled': False})
    assert agent.tick()['phase'] == 'disabled'
    assert not calls
    policy.write({**policy.DEFAULTS, 'manager_enabled': True})
    agent.tick()
    assert calls


def test_default_boot_autostart_does_not_resurrect_exited_app_even_after_agent_restart(isolated):
    calls, current = isolated
    runtime_state.set_vision_autostart('demo', True)
    runtime_state.mark_vision_started('demo', 'production.json')
    agent = manager.Manager()
    agent.tick()
    current['value'] = None
    assert agent.tick()['phase'] == 'stopped'
    assert manager.Manager().tick()['phase'] == 'stopped'
    assert len(calls) == 1


def test_first_autostart_is_not_a_fault_but_later_restorations_are(isolated):
    from rk3588_runtime import recovery_history
    _, current = isolated
    runtime_state.set_vision_autostart('demo', True)
    runtime_state.mark_vision_started('demo', 'production.json')
    policy.write({**policy.DEFAULTS, 'app_restart_enabled': True})
    agent = manager.Manager()
    agent.tick()
    current['value'] = None
    agent.tick()
    current['value'] = None
    agent.tick()
    restored = [event for event in recovery_history.snapshot('application')['events'] if event['kind'] == 'restored']
    assert len(restored) == 3
    assert [event['fault_recovery'] for event in restored] == [True, True, False]


def test_failed_start_uses_retry_backoff(isolated, monkeypatch):
    runtime_state.set_vision_autostart('demo', True)
    runtime_state.mark_vision_started('demo', 'production.json')
    attempts = []
    def fail(*args):
        attempts.append(args)
        raise RuntimeError('test launch failed')
    monkeypatch.setattr(pm, 'start_app', fail)
    clock = {'value': 100}
    monkeypatch.setattr(manager.time, 'monotonic', lambda: clock['value'])
    agent = manager.Manager()
    assert agent.tick()['retry_in_sec'] == 5
    agent.tick()
    assert len(attempts) == 1
    clock['value'] += 6
    assert agent.tick()['retry_in_sec'] == 10
    assert len(attempts) == 2


def test_policy_validation_and_conflicting_edits(isolated):
    original = policy.read()
    revision = policy.revision(original)
    policy.write({**original, 'restart_delay_sec': 15}, revision)
    with pytest.raises(policy.PolicyConflict):
        policy.write(original, revision)
    for bad in ({'task_timeout_sec': 1}, {'kernel_reboot_enabled': 'true'}, {'arbitrary_command': 'reboot'}):
        with pytest.raises(ValueError):
            policy.write(bad)
    policy.POLICY_FILE.write_text('{broken')
    with pytest.raises(ValueError):
        policy.read()


def test_saved_policy_controls_actual_service_properties_and_engine_deadlines(isolated, tmp_path, monkeypatch):
    app = pm.APPS_ROOT / 'demo'
    app.mkdir(parents=True)
    value = policy.write({**policy.DEFAULTS, 'restart_delay_sec': 15, 'task_timeout_sec': 180, 'startup_timeout_sec': 360})
    calls = []
    def run(command, **kwargs):
        calls.append(command)
        return subprocess.CompletedProcess(command, 0, '', '')
    def unexpected(*args):
        pytest.fail('disabled watchdog must not probe old executables')
    monkeypatch.setattr(pm.subprocess, 'run', run)
    monkeypatch.setattr(pm, '_follow_journal', lambda *args: None)
    monkeypatch.setattr(pm, '_supports_service_watchdog', unexpected)
    pm._start_systemd_app(app / 'vision_analysis', app / 'config.json', app, {}, pm._app_unit_name('demo'))
    assert '--property=Restart=no' in calls[0] and '--property=RestartSec=15' in calls[0]
    assert not any(item.startswith('--property=WatchdogSec=') for item in calls[0])
    assert '--setenv=RK_SERVICE_TASK_TIMEOUT_MS=180000' in calls[0]
    assert '--setenv=RK_SERVICE_STARTUP_TIMEOUT_MS=360000' in calls[0]
    assert (app / 'run.recovery_policy').read_text() == policy.app_revision(value)


def test_explicit_app_start_retires_only_our_live_disable_override(isolated, tmp_path, monkeypatch):
    monkeypatch.setattr(pm, 'SYSTEMD_RUNTIME_UNIT_DIR', tmp_path / 'units')
    unit = pm._app_unit_name('demo')
    directory = pm.SYSTEMD_RUNTIME_UNIT_DIR / (unit + '.d')
    directory.mkdir(parents=True)
    override = directory / '90-device-only-recovery.conf'
    custom = directory / '99-admin.conf'
    override.write_text(pm.DEVICE_ONLY_OVERRIDE)
    custom.write_text('[Service]\nMemoryMax=512M\n')
    app = pm.APPS_ROOT / 'demo'
    app.mkdir(parents=True)
    policy.write({**policy.DEFAULTS, 'app_restart_enabled': True, 'app_stall_enabled': True})
    calls = []
    monkeypatch.setattr(pm.subprocess, 'run', lambda command, **kwargs: calls.append(command) or subprocess.CompletedProcess(command, 0, '', ''))
    monkeypatch.setattr(pm, '_supports_service_watchdog', lambda *args: True)
    monkeypatch.setattr(pm, '_follow_journal', lambda *args: None)
    pm._start_systemd_app(app / 'vision_analysis', app / 'config.json', app, {}, unit)
    assert not override.exists() and custom.read_text() == '[Service]\nMemoryMax=512M\n'
    assert calls[0] == ['systemctl', 'daemon-reload']
    assert '--property=Restart=always' in calls[1] and '--property=WatchdogSec=30' in calls[1]


def test_live_disable_override_is_not_deleted_if_custom_or_reload_fails(tmp_path, monkeypatch):
    monkeypatch.setattr(pm, 'SYSTEMD_RUNTIME_UNIT_DIR', tmp_path)
    unit = pm._app_unit_name('demo')
    override = tmp_path / (unit + '.d') / '90-device-only-recovery.conf'
    override.parent.mkdir()
    override.write_text('[Service]\nRestart=on-failure\n')
    with pytest.raises(RuntimeError, match='自定义'):
        pm._clear_device_only_override(unit)
    assert override.read_text() == '[Service]\nRestart=on-failure\n'
    override.write_text(pm.DEVICE_ONLY_OVERRIDE)
    monkeypatch.setattr(pm.subprocess, 'run', lambda command, **kwargs: subprocess.CompletedProcess(command, 1, '', 'test failure'))
    with pytest.raises(RuntimeError, match='test failure'):
        pm._clear_device_only_override(unit)
    assert override.read_text() == pm.DEVICE_ONLY_OVERRIDE
    with pytest.raises(ValueError):
        pm._clear_device_only_override('../NetworkManager.service')


def test_systemd_recovery_events_survive_agent_restart_without_duplicates(isolated, monkeypatch):
    calls, current = isolated
    current['value'] = {'app': 'demo', 'config': 'production.json', 'pid': 222}
    monkeypatch.setattr(pm, 'get_status', lambda app: {
        'status': 'running', 'pid': 222, 'config': 'production.json', 'restart_count': 1})
    agent = manager.Manager()
    agent.tick()
    events = manager.read_json(manager.EVENTS_FILE, [])
    assert len(events) == 1 and events[0]['kind'] == 'application_restart'
    manager.Manager().tick()
    assert manager.read_json(manager.EVENTS_FILE, []) == events
    assert not calls


def test_management_never_treats_stale_status_as_live_heartbeat(isolated, tmp_path, monkeypatch):
    from rk3588_runtime import management
    import time
    units = {'active': True}
    monkeypatch.setattr(management, 'unit_status', lambda unit: {
        'installed': True, 'active': units['active'], 'enabled': True, 'state': 'active', 'error': None})
    monkeypatch.setattr(management, 'hardware_status', lambda: {'enabled': False})
    kernel = tmp_path / 'kernel-state.json'
    monkeypatch.setenv('KERNEL_RECOVERY_STATE_FILE', str(kernel))
    kernel.write_text(json.dumps({'reboots': [time.time()]}))
    state = {'boot_id': 'test-boot', 'updated_at': time.time() - 60}
    manager.atomic_json(manager.STATUS_FILE, state)
    snapshot = management.snapshot()
    assert not snapshot['agent']['heartbeat_fresh'] and snapshot['runtime'] is None
    assert snapshot['events'][0]['kind'] == 'board_reboot_request'
    state['updated_at'] = time.time()
    manager.atomic_json(manager.STATUS_FILE, state)
    assert management.snapshot()['agent']['heartbeat_fresh']
    units['active'] = False
    assert not management.snapshot()['agent']['heartbeat_fresh']


def test_staged_platform_imports_without_web_or_third_party_modules(tmp_path):
    installer = Path(__file__).resolve().parents[1] / 'install_runtime.py'
    subprocess.run([sys.executable, str(installer), '--stage', str(tmp_path)], check=True)
    installed = tmp_path / 'usr/lib/vision-analysis-platform/python'
    # An isolated interpreter cannot accidentally find the source Web package.
    code = '''
import sys
sys.path.insert(0, sys.argv[1])
from rk3588_runtime import manager, management, process_manager, recovery_policy, network_recovery, network_policy, performance
assert not any(name in sys.modules for name in ('fastapi', 'pydantic', 'uvicorn', 'routers', 'services'))
print('independent runtime import passed')
'''
    result = subprocess.run([sys.executable, '-I', '-c', code, str(installed)],
                            check=True, capture_output=True, text=True)
    assert 'independent runtime import passed' in result.stdout
    unit = (tmp_path / 'lib/systemd/system/rk3588-runtime-manager.service').read_text()
    assert '_console' not in unit and 'web_console' not in unit
    assert 'Before=rk3588-console.service' in unit and 'Requires=rk3588-console.service' not in unit
    performance_unit = (tmp_path / 'lib/systemd/system/rk3588-performance.service').read_text()
    assert 'Type=oneshot' in performance_unit and 'rk3588_runtime.performance' in performance_unit
    assert 'Before=rk3588-console.service rk3588-runtime-manager.service' in performance_unit
    network_unit = (tmp_path / 'lib/systemd/system/rk3588-network-recovery.service').read_text()
    assert 'rk3588_runtime.network_recovery' in network_unit and 'WatchdogSec=120' in network_unit
    assert 'web_console' not in network_unit and 'Requires=rk3588-console.service' not in network_unit
    history_unit = (tmp_path / 'lib/systemd/system/rk3588-recovery-history.service').read_text()
    assert 'rk3588_runtime.recovery_observer' in history_unit and 'WatchdogSec=120' in history_unit
    journal_limits = (tmp_path / 'etc/systemd/journald.conf.d/zz-rk3588-bounded-logs.conf').read_text()
    assert 'SystemMaxUse=64M' in journal_limits and 'RuntimeMaxUse=32M' in journal_limits
    assert 'Storage=' not in journal_limits
    assert (tmp_path / 'lib/systemd/system/rk3588-console.service.d/90-bounded-dumps.conf').read_text().endswith('LimitCORE=16M\n')


def test_package_copy_failure_does_not_stop_running_application(isolated, tmp_path, monkeypatch):
    from rk3588_runtime import package_install
    source = tmp_path / 'package'
    source.mkdir()
    (source / pm.BINARY_NAME).write_bytes(b'binary')
    stopped = []
    monkeypatch.setattr(pm, 'stop_all_apps_for_install_unlocked', lambda: stopped.append(True))
    def fail(*args, **kwargs):
        raise OSError('test disk full')
    monkeypatch.setattr(package_install.shutil, 'copytree', fail)
    with pytest.raises(OSError, match='disk full'):
        package_install.install(source, 'demo')
    assert not stopped


def test_package_install_stops_under_runtime_lock_and_preserves_external_data(isolated, tmp_path, monkeypatch):
    from rk3588_runtime import package_install
    from contextlib import contextmanager
    source = tmp_path / 'package'
    source.mkdir()
    (source / pm.BINARY_NAME).write_bytes(b'new')
    destination = pm.APPS_ROOT / 'demo'
    destination.mkdir(parents=True)
    (destination / pm.BINARY_NAME).write_bytes(b'old')
    external = pm.APPS_ROOT / '.data/demo/event_store'
    external.mkdir(parents=True)
    (external / 'keep').write_bytes(b'event')
    held = []
    @contextmanager
    def lock():
        held.append(True)
        yield
        held.pop()
    def stop():
        assert held and (destination / pm.BINARY_NAME).read_bytes() == b'old'
    monkeypatch.setattr(pm, 'runtime_lock', lock)
    monkeypatch.setattr(pm, 'stop_all_apps_for_install_unlocked', stop)
    package_install.install(source, 'demo')
    assert (destination / pm.BINARY_NAME).read_bytes() == b'new'
    assert (external / 'keep').read_bytes() == b'event'
