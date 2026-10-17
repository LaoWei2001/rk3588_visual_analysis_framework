"""Opt-in real systemd tests. Only disposable test processes are killed.

RK3588_SYSTEMD_TESTS=1 python3 -m pytest services/framework/system_watchdog/tests/test_systemd_recovery_integration.py -q
"""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

import pytest

pytestmark = pytest.mark.skipif(os.environ.get('RK3588_SYSTEMD_TESTS') != '1', reason='requires opt-in local systemd')
ROOT = Path(__file__).resolve().parents[4]


def wait_for(predicate, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.1)
    raise AssertionError('recovery condition did not become true before timeout')


@pytest.fixture
def managed_app(tmp_path, monkeypatch):
    sys.path.insert(0, str(ROOT / 'web_console/backend'))
    from services import process_manager as pm, runtime_state
    app = tmp_path / 'recovery-integration'
    (app / 'assets').mkdir(parents=True)
    source = tmp_path / 'fixture.cpp'
    source.write_text(r'''
#include "runtime/service_watchdog.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--watchdog-protocol") == 0) {
        std::puts("rk3588-service-watchdog-v1"); return 0;
    }
    std::string mode; std::ifstream(argv[1]) >> mode;
    std::puts("recovery-fixture-started"); std::fflush(stdout);
    service_watchdog::start();
    service_watchdog::Progress progress("stalled inference", 2, 400);
    if (mode == "stall") progress.begin();
    while (true) { service_watchdog::main_heartbeat(); usleep(100000); }
}
''')
    subprocess.run(['g++', '-std=c++14', '-pthread', '-I', str(ROOT / 'engine/src'),
                    str(source), str(ROOT / 'engine/src/runtime/service_watchdog.cpp'),
                    '-o', str(app / 'vision_analysis')], check=True, timeout=30)
    (app / 'assets/config.json').write_text('idle')
    monkeypatch.setattr(pm, 'APPS_ROOT', tmp_path)
    monkeypatch.setattr(pm, '_start_lock_path', tmp_path / '.start.lock')
    monkeypatch.setattr(pm, '_processes', {})
    monkeypatch.setattr(pm, 'data_dir', lambda name: tmp_path / '.data' / name)
    monkeypatch.setattr(runtime_state, 'STATE_FILE', tmp_path / 'runtime.json')
    monkeypatch.setattr(runtime_state, 'LOCK_FILE', tmp_path / 'runtime.lock')
    monkeypatch.setattr(pm.recovery_policy, 'POLICY_FILE', tmp_path / 'policy.json')
    pm.recovery_policy.write({**pm.recovery_policy.DEFAULTS, 'app_restart_enabled': True, 'app_stall_enabled': True})
    from services.dataset_manager import dataset_manager
    monkeypatch.setattr(dataset_manager, 'pause_app', lambda name: None)
    yield pm, app
    pm.stop_app(app.name)
    sys.path.remove(str(ROOT / 'web_console/backend'))


def test_crash_recovery_pid_logs_singleton_and_manual_stop(managed_app):
    pm, app = managed_app
    first = pm.start_app(app.name)
    os.kill(first, signal.SIGKILL)
    wait_for(lambda: pm.get_status(app.name).get('recovery_state') == 'restarting')
    other = app.parent / 'other-app'
    (other / 'assets').mkdir(parents=True)
    shutil.copy2(app / 'vision_analysis', other / 'vision_analysis')
    (other / 'assets/config.json').write_text('idle')
    with pytest.raises(pm.AppAlreadyRunningError):
        pm.start_app(other.name)
    status = wait_for(lambda: (lambda s: s if s.get('pid') and s['pid'] != first else None)(pm.get_status(app.name)))
    assert status['restart_count'] >= 1
    # The journal must contain output from both the first and the restarted process.
    output = subprocess.run(['journalctl', '-u', pm._app_unit_name(app.name), '--output=cat', '--no-pager'],
                            capture_output=True, text=True, check=True).stdout
    assert output.count('recovery-fixture-started') >= 2
    # Simulate losing the console's in-memory registry and old journal follower.
    follower = pm._processes[app.name].launcher
    follower.terminate()
    follower.wait(timeout=3)
    pm._processes.clear()
    pm.recover_processes()
    assert pm.get_status(app.name)['pid'] == status['pid']
    assert pm._processes[app.name].launcher is not None
    assert pm.stop_app(app.name)
    time.sleep(6)  # Longer than RestartSec: manual stop must not resurrect it.
    assert pm.get_status(app.name)['status'] == 'stopped'


def test_real_engine_watchdog_kills_stalled_worker(managed_app, monkeypatch):
    pm, app = managed_app
    (app / 'assets/config.json').write_text('stall')
    original = subprocess.run
    def run(command, **kwargs):
        if command[0] == 'systemd-run':
            command = [arg.replace('--property=WatchdogSec=30', '--property=WatchdogSec=2') for arg in command]
        return original(command, **kwargs)
    monkeypatch.setattr(pm.subprocess, 'run', run)
    first = pm.start_app(app.name)
    status = wait_for(lambda: (lambda s: s if s.get('pid') and s['pid'] != first else None)(pm.get_status(app.name)))
    assert status['restart_count'] >= 1
    output = original(['journalctl', '-u', pm._app_unit_name(app.name), '--output=cat', '--no-pager'],
                      capture_output=True, text=True, check=True).stdout
    assert 'stalled inference channel=2 stalled' in output


def test_program_start_retires_old_disable_override_and_enables_selected_recovery(managed_app):
    pm, app = managed_app
    unit = pm._app_unit_name(app.name)
    override = pm.SYSTEMD_RUNTIME_UNIT_DIR / (unit + '.d') / '90-device-only-recovery.conf'
    override.parent.mkdir(parents=True, exist_ok=True)
    assert not override.exists()
    override.write_text(pm.DEVICE_ONLY_OVERRIDE)
    try:
        first = pm.start_app(app.name)
        assert not override.exists()
        status = pm.get_status(app.name)
        assert status['restart_enabled'] and status['watchdog_active']
        os.kill(first, signal.SIGKILL)
        restarted = wait_for(lambda: (lambda s: s if s.get('pid') and s['pid'] != first else None)(pm.get_status(app.name)))
        assert restarted['restart_count'] >= 1
    finally:
        override.unlink(missing_ok=True)


def test_console_event_loop_stall_is_restarted():
    unit = 'rk3588-recovery-console-integration.service'
    code = '''
import asyncio, time
from services.service_watchdog import heartbeat
async def main():
    ready = asyncio.Event()
    ready.set()
    asyncio.create_task(heartbeat(ready))
    await asyncio.sleep(.2)
    time.sleep(30)
asyncio.run(main())
'''
    subprocess.run(['systemd-run', '--quiet', '--collect', '--unit=' + unit,
                    '--service-type=exec', '--property=Restart=always', '--property=RestartSec=1',
                    '--property=StartLimitIntervalSec=0', '--property=WatchdogSec=2',
                    '--property=WatchdogSignal=SIGKILL', '--property=TimeoutStopSec=3',
                    '--setenv=PYTHONPATH=' + str(ROOT / 'web_console/backend'),
                    '/usr/bin/python3', '-c', code], check=True, timeout=10)
    try:
        def restarted():
            result = subprocess.run(['systemctl', 'show', unit, '-p', 'NRestarts', '--value'],
                                    capture_output=True, text=True, check=True)
            return int(result.stdout.strip() or '0') > 0
        wait_for(restarted)
    finally:
        subprocess.run(['systemctl', 'stop', unit], check=True, timeout=10)


def test_packaged_runtime_restores_and_recovers_app_without_web(managed_app):
    pm, app = managed_app
    from services import runtime_state
    runtime_state.set_vision_autostart(app.name, True)
    runtime_state.mark_vision_started(app.name, 'config.json')
    stage = app.parent / 'isolated-platform'
    subprocess.run([sys.executable, str(ROOT / 'services/framework/runtime_manager/install_runtime.py'),
                    '--stage', str(stage)], check=True, timeout=10)
    library = stage / 'usr/lib/vision-analysis-platform/python'
    status_file = app.parent / 'agent-status.json'
    unit = 'rk3588-runtime-isolated-integration.service'
    environment = {
        'PYTHONPATH': str(library), 'APPS_ROOT': str(app.parent),
        'RUNTIME_STATE_FILE': str(runtime_state.STATE_FILE),
        'RUNTIME_POLICY_FILE': str(app.parent / 'policy.json'),
        'RUNTIME_STATUS_FILE': str(status_file), 'RUNTIME_EVENTS_FILE': str(app.parent / 'events.json'),
    }
    # This test deliberately opts into legacy application recovery.
    Path(environment['RUNTIME_POLICY_FILE']).write_text(pm.recovery_policy.POLICY_FILE.read_text())
    command = ['systemd-run', '--quiet', '--collect', '--unit=' + unit, '--service-type=exec',
               '--property=Restart=no', '--property=WatchdogSec=60', '--property=TimeoutStopSec=5']
    command += [f'--setenv={key}={value}' for key, value in environment.items()]
    command += ['/usr/bin/python3', '-m', 'rk3588_runtime.manager']
    subprocess.run(command, check=True, timeout=10)
    try:
        first = wait_for(lambda: pm._systemd_main_pid(pm._app_unit_name(app.name)))
        import json
        def restored():
            try:
                return json.loads(status_file.read_text()).get('current', {}).get('app') == app.name
            except (OSError, ValueError, AttributeError):
                return False
        wait_for(restored)
        os.kill(first, signal.SIGKILL)
        second = wait_for(lambda: (lambda pid: pid if pid and pid != first else None)(
            pm._systemd_main_pid(pm._app_unit_name(app.name))))
        assert second != first
        def recorded():
            try:
                events = json.loads((app.parent / 'events.json').read_text())
                return any(event.get('kind') == 'application_restart' for event in events)
            except (OSError, ValueError):
                return False
        wait_for(recorded)
        # Stopping the supervisor does not stop its independently owned application.
        subprocess.run(['systemctl', 'stop', unit], check=True, timeout=10)
        assert pm._systemd_main_pid(pm._app_unit_name(app.name)) == second
        assert not str(ROOT / 'web_console') in environment['PYTHONPATH']
    finally:
        subprocess.run(['systemctl', 'stop', unit], check=False, timeout=10)
