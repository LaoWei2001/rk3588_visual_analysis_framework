"""No device operations: concurrency, durable limits, migration and escalation."""
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

from rk3588_runtime import reboot_coordinator as reboot, recovery_history as audit, recovery_policy, network_policy


@pytest.fixture
def board(tmp_path, monkeypatch):
    clock = SimpleNamespace(utc=100000, mono=2000, boot='a')
    monkeypatch.setattr(reboot, 'time', SimpleNamespace(time=lambda: clock.utc, monotonic=lambda: clock.mono))
    monkeypatch.setattr(audit, 'boot_id', lambda: clock.boot)
    monkeypatch.setattr(recovery_policy, 'POLICY_FILE', tmp_path / 'system.json')
    monkeypatch.setattr(network_policy, 'POLICY_FILE', tmp_path / 'network.json')
    recovery_policy.write({**recovery_policy.DEFAULTS, 'kernel_reboot_enabled': True})
    network_policy.write({**network_policy.DEFAULTS, 'enabled': True, 'allow_reboot': True,
                          'targets': [{'host': '192.0.2.10', 'method': 'ping'}]}, network_policy.revision(network_policy.read()))
    calls = []
    def submit(force):
        state = json.loads(reboot.STATE_FILE.read_text())
        assert state['pending']['forced'] == force
        assert state['attempts']
        calls.append(force)
    monkeypatch.setattr(reboot, '_submit', submit)
    return clock, calls


def request(source, **kwargs):
    with reboot.guard() as coordinator:
        if coordinator is None:
            return {'status': 'busy'}
        return coordinator.request(source, source + ' reason', **kwargs)


def test_concurrent_sources_and_service_restart_submit_once_and_merge_history(board):
    _, calls = board
    with ThreadPoolExecutor(max_workers=3) as workers:
        list(workers.map(request, ('network', 'kernel', 'scheduled')))
    for source in ('network', 'kernel', 'scheduled'):
        assert request(source)['status'] == 'pending'
    assert calls == [False]
    state = json.loads(reboot.STATE_FILE.read_text())
    assert len(state['attempts']) == 1 and len(state['pending']['reasons']) == 3
    rows = audit.snapshot()['events']
    assert len(rows) == 1 and '合并重启原因' in rows[0]['detail']


def test_kernel_escalates_only_after_grace_and_never_reserves_twice(board):
    clock, calls = board
    first = request('network')
    clock.mono += 119
    assert request('kernel')['status'] == 'pending'
    clock.mono += 1
    assert request('kernel')['status'] == 'escalated'
    clock.mono += 400
    for source in ('network', 'scheduled', 'kernel'):
        assert request(source)['event_id'] == first['event_id']
    assert calls == [False, True]
    assert len(json.loads(reboot.STATE_FILE.read_text())['attempts']) == 1
    event = audit.snapshot()['events'][0]
    assert '120 秒' in event['detail'] and '--force' in event['action']


def test_fresh_kernel_fault_can_escalate_even_after_ordinary_pending_expiry(board):
    clock, calls = board
    request('scheduled')
    clock.mono += 400
    assert request('kernel')['status'] == 'escalated'
    assert calls == [False, True]


def test_disabled_kernel_policy_cannot_force_a_pending_network_reboot(board):
    clock, calls = board
    request('network')
    clock.mono += 121
    recovery_policy.write({**recovery_policy.read(), 'kernel_reboot_enabled': False})
    assert request('kernel')['status'] == 'disabled'
    assert calls == [False]


def test_shared_limits_survive_boots_take_stricter_policy_and_count_only_faults(board):
    clock, calls = board
    assert reboot._limits('kernel')[1:] == (3600, 2)
    assert request('kernel')['status'] == 'submitted'
    clock.boot = 'b'
    clock.utc += 1800
    assert request('network')['status'] == 'limited'
    clock.utc += 1800
    assert request('network')['status'] == 'submitted'
    clock.boot = 'c'
    clock.utc += 3600
    assert request('kernel')['status'] == 'limited'
    assert request('scheduled')['status'] == 'submitted'
    clock.boot = 'd'
    assert request('scheduled')['status'] == 'limited'  # Also observes the shared cooldown.
    clock.utc += 86400
    assert request('kernel')['status'] == 'submitted'
    assert calls == [False] * 4


def test_backwards_wall_clock_keeps_shared_cooldown(board):
    clock, calls = board
    request('network')
    clock.boot = 'b'
    clock.utc -= 100000
    assert request('kernel')['status'] == 'limited'
    assert calls == [False]


def test_upgrade_imports_legacy_attempts_once_and_rejects_corruption(board):
    clock, calls = board
    for path in reboot.LEGACY_FILES:
        path.write_text(json.dumps({'reboots': [clock.utc - 4000]}))
    assert request('network')['status'] == 'limited'
    assert not calls
    reboot.LEGACY_FILES[0].write_text('{broken')
    assert request('kernel')['status'] == 'limited'  # Legacy history is not reimported.
    reboot.STATE_FILE.unlink()
    with pytest.raises(ValueError):
        request('scheduled')


def test_network_repair_and_scheduled_reboot_use_same_process_lock(board):
    _, calls = board
    with reboot.guard() as coordinator:
        assert coordinator is not None
        assert request('scheduled')['status'] == 'busy'
    assert not calls
    assert request('scheduled')['status'] == 'submitted'


@pytest.mark.parametrize('payload', ['{broken', '{"version":1,"attempts":[{"time":true,"fault":true}]}',
                                   '{"version":1,"attempts":[],"pending":{"forced":false}}'])
def test_corrupt_shared_state_never_authorizes_commands(board, payload):
    reboot.STATE_FILE.parent.mkdir(parents=True)
    reboot.STATE_FILE.write_text(payload)
    with pytest.raises(ValueError):
        request('network')
    assert not board[1]


def test_persistence_failure_never_executes_reboot(board, monkeypatch):
    monkeypatch.setattr(reboot, '_write', lambda *_: (_ for _ in ()).throw(OSError('disk full')))
    with pytest.raises(OSError):
        request('network')
    assert not board[1]


def test_source_persistence_failure_keeps_limit_without_executing(board):
    def failed(*args):
        raise OSError('source state disk full')
    with pytest.raises(OSError):
        request('kernel', on_accept=failed)
    state = json.loads(reboot.STATE_FILE.read_text())
    assert not state['pending'] and len(state['attempts']) == 1 and not board[1]
    assert request('network')['status'] == 'limited'


@pytest.mark.parametrize('timeout', [False, True])
def test_command_failure_keeps_budget_and_timeout_keeps_pending(board, monkeypatch, timeout):
    def fail(force):
        if timeout:
            raise subprocess.TimeoutExpired(['systemctl', 'reboot'], 15)
        raise subprocess.CalledProcessError(1, ['systemctl', 'reboot'])
    monkeypatch.setattr(reboot, '_submit', fail)
    with pytest.raises(subprocess.SubprocessError):
        request('network')
    state = json.loads(reboot.STATE_FILE.read_text())
    assert len(state['attempts']) == 1
    assert bool(state['pending']) == timeout
    assert request('scheduled')['status'] == ('pending' if timeout else 'limited')


def test_stale_normal_request_releases_repairs_without_resetting_cooldown(board):
    clock, calls = board
    request('network')
    clock.mono += 300
    with reboot.guard() as coordinator:
        assert coordinator.pending is None
    assert request('network')['status'] == 'limited'
    assert calls == [False]


def test_shutdown_in_progress_never_submits_another_command(board, monkeypatch):
    monkeypatch.setattr(reboot, '_system_state', lambda: 'stopping')
    assert request('network')['status'] == 'stopping'
    assert not board[1] and not reboot.STATE_FILE.exists()


def test_scheduled_cli_without_web_uses_durable_shared_state_and_merges_in_subprocess(tmp_path):
    command = tmp_path / 'fake-systemctl'
    log = tmp_path / 'commands.json'
    state = tmp_path / 'shared/state.json'
    command.write_text('#!/usr/bin/python3\nimport json,os,sys\nfrom pathlib import Path\n'
                       'if sys.argv[1:] == ["is-system-running"]: print("running")\n'
                       'elif sys.argv[1:] == ["reboot"]:\n'
                       ' state=json.loads(Path(os.environ["REBOOT_COORDINATOR_STATE_FILE"]).read_text())\n'
                       ' assert state["pending"] and len(state["attempts"]) == 1\n'
                       ' with open(os.environ["TEST_COMMAND_LOG"],"a") as f: f.write("reboot\\n")\n'
                       'else: sys.exit(9)\n')
    command.chmod(0o755)
    environment = {**os.environ, 'PYTHONPATH': str(Path(__file__).parents[1] / 'python'),
                   'REBOOT_COORDINATOR_STATE_FILE': str(state), 'SYSTEMCTL_PATH': str(command),
                   'RUNTIME_POLICY_FILE': str(tmp_path / 'policy.json'),
                   'NETWORK_RECOVERY_POLICY_FILE': str(tmp_path / 'network-policy.json'),
                   'NETWORK_RECOVERY_STATE_FILE': str(tmp_path / 'network-state.json'),
                   'KERNEL_RECOVERY_STATE_FILE': str(tmp_path / 'kernel-state.json'),
                   'RECOVERY_HISTORY_FILE': str(tmp_path / 'history/events.json'),
                   'TEST_COMMAND_LOG': str(log)}
    statuses = []
    for _ in range(2):
        result = subprocess.run([sys.executable, '-m', 'rk3588_runtime.reboot_coordinator', '--scheduled'],
                                env=environment, check=True, capture_output=True, text=True, timeout=10)
        statuses.append(json.loads(result.stdout)['status'])
    assert statuses == ['submitted', 'pending']
    assert log.read_text() == 'reboot\n'
    assert len(json.loads((tmp_path / 'history/events.json').read_text())['events']) == 1


@pytest.mark.parametrize('recording', [False, True])
def test_installer_migrates_old_daily_units_idempotently_without_changing_timer(tmp_path, recording):
    spec = importlib.util.spec_from_file_location('runtime_installer', Path(__file__).parents[1] / 'install_runtime.py')
    installer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(installer)
    path = tmp_path / 'scheduled.service'
    path.write_text('[Unit]\nDescription=RK3588 Daily Scheduled Reboot\n[Service]\nType=oneshot\n'
                    + ('Environment=PYTHONPATH=/usr/lib/vision-analysis-platform/python\n'
                       'ExecStartPre=/usr/bin/python3 -m rk3588_runtime.recovery_observer --record-scheduled-reboot\n'
                       'ExecStopPost=/usr/bin/python3 -m rk3588_runtime.recovery_observer --scheduled-result\n' if recording else '')
                    + 'ExecStart=/usr/bin/systemctl reboot\n')
    assert installer.upgrade_daily_reboot(path)
    content = path.read_text()
    assert 'rk3588_runtime.reboot_coordinator --scheduled' in content
    assert '--record-scheduled-reboot' not in content and '--scheduled-result' not in content
    assert content.count('Environment=PYTHONPATH=') == 1
    assert not installer.upgrade_daily_reboot(path)
    assert path.read_text() == content


def test_installer_preserves_custom_reboot_command(tmp_path):
    spec = importlib.util.spec_from_file_location('runtime_installer', Path(__file__).parents[1] / 'install_runtime.py')
    installer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(installer)
    path = tmp_path / 'custom.service'
    content = '[Unit]\nDescription=RK3588 Daily Scheduled Reboot\n[Service]\nExecStart=/usr/bin/systemctl reboot --force\n'
    path.write_text(content)
    assert not installer.upgrade_daily_reboot(path)
    assert path.read_text() == content
