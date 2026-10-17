"""Exercise recovery policy and failures without touching host networking."""
import copy
import fcntl
import json
from pathlib import Path
import socket
import subprocess
import sys
import threading

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from rk3588_runtime import network_policy as policy, network_recovery as recovery

CONNECTION = '01234567-89ab-cdef-0123-456789abcdef'


def settings(**changes):
    return policy.validate({**policy.DEFAULTS, 'enabled': True,
                            'targets': [{'host': '192.0.2.10', 'port': 443}], **changes})


def observation(**changes):
    return {'interface': 'wlan0', 'kind': 'wifi', 'present': True, 'carrier': True,
            'manager_ok': True, 'managed': True, 'addresses': ['192.0.2.2'], 'has_route': True,
            'gateway': '192.0.2.1', 'gateway_reachable': True, 'connection_uuid': CONNECTION,
            'reachable': True, 'targets': [], **changes}


def healthy(config=None):
    return recovery.evaluate(config or settings(), observation(), {}, 200, 10000, 'boot')[0]


def failed(**changes):
    return observation(reachable=False, gateway_reachable=False, **changes)


@pytest.mark.parametrize('bad', [
    {'enabled': True}, {'targets': [{'host': 'https://example.com/', 'port': 443}]},
    {'targets': [{'host': '127.0.0.1', 'port': 22}]}, {'targets': [{'host': '::1', 'port': 22}]},
    {'targets': [{'host': 'example.com; reboot', 'port': 22}]},
    {'targets': [{'host': 'example.com', 'port': True}]},
    {'targets': [{'host': 'example.com', 'port': 0}]},
    {'targets': [{'host': '192.0.2.10', 'method': 'udp'}]},
    {'targets': [{'host': '192.0.2.10', 'method': 'ping', 'port': 443}]},
    {'targets': [{'host': '127.0.0.1', 'method': 'ping'}]},
    {'targets': [{'host': '192.0.2.10', 'method': 'tcp'}]},
    {'targets': [{'host': '192.0.2.10', 'method': 'ping', 'command': 'reboot'}]},
    {'interface': 'wlan0;reboot'}, {'interface': 'lo'}, {'allow_reboot': 'yes'},
    {'reboot_cooldown_sec': 0}, {'max_reboots_per_day': 100}, {'shell_command': 'reboot'},
    {'failure_timeout_sec': 1800, 'reboot_after_sec': 300},
])
def test_invalid_policy_cannot_enable_unsafe_recovery(bad):
    with pytest.raises(ValueError):
        policy.validate(bad)


def test_policy_cas_and_target_normalization(tmp_path, monkeypatch):
    monkeypatch.setattr(policy, 'POLICY_FILE', tmp_path / 'policy.json')
    original = policy.read()
    fingerprint = policy.revision(original)
    new = settings(targets=[{'host': 'Example.COM', 'port': 443}, {'host': 'example.com', 'port': 443}])
    policy.write(new, fingerprint)
    assert policy.read()['targets'] == [{'host': 'example.com', 'port': 443}]
    with pytest.raises(policy.PolicyConflict):
        policy.write(original, fingerprint)
    assert policy.read()['enabled']


def test_mixed_ping_tcp_targets_and_legacy_revision_are_preserved():
    legacy = settings()
    assert policy.revision(policy.validate(legacy)) == policy.revision(legacy)
    explicit_tcp = settings(targets=[{**legacy['targets'][0], 'method': 'tcp'}])
    assert policy.revision(explicit_tcp) == policy.revision(legacy)
    mixed = settings(targets=[{'host': '192.0.2.20', 'method': 'ping'},
                              {'host': 'Example.COM', 'method': 'tcp', 'port': 8443},
                              {'host': '192.0.2.20', 'method': 'ping'}])
    assert mixed['targets'] == [{'host': '192.0.2.20', 'method': 'ping'},
                                {'host': 'example.com', 'port': 8443}]
    state = healthy(legacy)
    _, status, action = recovery.evaluate(mixed, failed(), state, 9000, 20000, 'boot')
    assert status['phase'] == 'waiting_baseline' and action is None


def test_no_baseline_or_disabled_policy_never_attempts_recovery():
    for config in (settings(), settings(enabled=False)):
        state, status, action = recovery.evaluate(config, failed(), {}, 9000, 20000, 'boot')
        assert not action and not state['armed']
        assert status['phase'] in ('waiting_baseline', 'disabled')


@pytest.mark.parametrize('change,phase', [
    ({'gateway_reachable': True}, 'upstream_unreachable'),
    ({'kind': 'ethernet', 'carrier': False}, 'link_down'),
    ({'managed': False}, 'waiting_interface'),
])
def test_upstream_outage_unplugged_cable_and_unmanaged_device_do_not_trigger_actions(change, phase):
    config = settings(allow_reboot=True)
    state = healthy(config)
    obs = failed()
    obs.update(change)
    for now in (300, 1000, 5000):
        state, status, action = recovery.evaluate(config, obs, state, now, 10000 + now, 'boot')
        assert not action and status['phase'] == phase


def test_graded_recovery_waits_cooldowns_and_reboot_is_recorded_before_request():
    config = settings(allow_reboot=True)
    state = healthy(config)
    for now, expected in [(300, None), (479, None), (480, 'reconnect'), (500, None),
                          (779, None), (780, 'restart_manager'), (900, None), (1200, 'reboot')]:
        state, status, action = recovery.evaluate(config, failed(), state, now, 10000 + now, 'boot')
        assert action == expected, (now, status)
    assert state['reboots'] == [11200]
    state, status, action = recovery.evaluate(config, failed(), state, 1215, 11215, 'boot')
    assert not action and status['phase'] == 'rate_limited'


def test_success_resets_fault_without_discarding_limits():
    state = healthy()
    for now in (300, 480):
        state, _, _ = recovery.evaluate(settings(), failed(), state, now, 10000 + now, 'boot')
    state, status, action = recovery.evaluate(settings(), observation(), state, 500, 10500, 'boot')
    assert status['phase'] == 'healthy' and not action
    assert state['stage'] == 0 and state['fault_since'] is None
    assert state['actions'] == [10480]


def test_baseline_survives_boot_with_same_policy_and_limits_are_retained():
    config = settings(allow_reboot=True)
    state = healthy(config)
    state.update(actions=[10000], reboots=[10000], stage=2, fault_since=200)
    state, status, action = recovery.evaluate(config, failed(), state, 20, 10020, 'new-boot')
    assert status['phase'] == 'boot_grace' and not action and state['armed']
    assert state['reboots'] == [10000] and state['actions'] == [10000]
    assert state['connection_uuid'] == CONNECTION


def test_changed_targets_require_new_success_instead_of_reusing_old_baseline():
    state = healthy()
    config = settings(targets=[{'host': '192.0.2.99', 'port': 22}])
    state, status, action = recovery.evaluate(config, failed(), state, 9000, 20000, 'boot')
    assert not action and not state['armed'] and status['phase'] == 'waiting_baseline'


def test_uncertain_gateway_does_not_authorize_board_reboot():
    config = settings(allow_reboot=True)
    state = healthy(config)
    state.update(stage=2, fault_since=200, actions=[15000], known_gateway='')
    _, status, action = recovery.evaluate(config, failed(gateway=''), state, 2000, 15100, 'boot')
    assert not action and not status['local_fault']


def test_backwards_clock_and_daily_reboot_limit_are_conservative():
    config = settings(allow_reboot=True)
    for attempts in ([20000], [10001, 10002]):
        state = healthy(config)
        state.update(stage=2, fault_since=200, reboots=attempts)
        _, status, action = recovery.evaluate(config, failed(), state, 2000, 15000, 'boot')
        assert not action and status['phase'] == 'rate_limited'


@pytest.fixture
def files(tmp_path, monkeypatch):
    monkeypatch.setattr(policy, 'POLICY_FILE', tmp_path / 'policy.json')
    monkeypatch.setattr(recovery, 'STATE_FILE', tmp_path / 'state.json')
    monkeypatch.setattr(recovery, 'STATUS_FILE', tmp_path / 'status.json')
    monkeypatch.setattr(recovery, 'TRANSACTION_DIR', tmp_path / 'transactions')
    return tmp_path


def test_network_guard_uses_same_flock_and_pauses_during_transactions(files):
    recovery.TRANSACTION_DIR.mkdir()
    with (recovery.TRANSACTION_DIR / '.lock').open('a+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with recovery.network_guard() as safe:
            assert not safe
    transaction = recovery.TRANSACTION_DIR / 'change.json'
    for value in ('{"status":"awaiting_confirmation"}', '{broken'):
        transaction.write_text(value)
        with recovery.network_guard() as safe:
            assert not safe
    transaction.write_text('{"status":"confirmed"}')
    with recovery.network_guard() as safe:
        assert safe


def test_state_is_durable_before_recovery_command(files, monkeypatch):
    import time
    config = settings()
    policy.write(config, policy.revision(policy.read()))
    monitor = recovery.Monitor()
    state = healthy(config)
    state.update(boot_id=monitor.boot_id, fault_since=time.monotonic()-200)
    monitor.state = state
    monkeypatch.setattr(recovery, 'observe', lambda *args: failed())
    monkeypatch.setattr(recovery, 'probe', lambda *args: {'reachable': False})
    calls = []
    def perform(action, state):
        from rk3588_runtime import recovery_history
        saved = recovery_history.snapshot('network')['events']
        assert len(saved) == 1 and saved[0]['result'] == 'pending'
        assert '持续不可达' in saved[0]['reason'] and '重新激活' in saved[0]['action']
        on_disk = json.loads(recovery.STATE_FILE.read_text())
        assert on_disk['stage'] == 1 and len(on_disk['actions']) == 1
        calls.append(action)
    monkeypatch.setattr(recovery, 'perform', perform)
    assert monitor.tick()['phase'] == 'recovering'
    assert calls == ['reconnect']
    from rk3588_runtime import recovery_history
    assert recovery_history.snapshot('network')['events'][0]['result'] == 'performed'
    monkeypatch.setattr(recovery, 'observe', lambda *args: observation())
    assert monitor.tick()['phase'] == 'healthy'
    assert recovery_history.snapshot('network')['events'][0]['result'] == 'recovered'


def test_disable_during_recheck_cancels_pending_action(files, monkeypatch):
    import time
    config = settings()
    policy.write(config, policy.revision(policy.read()))
    monitor = recovery.Monitor()
    monitor.state = healthy(config)
    monitor.state.update(boot_id=monitor.boot_id, fault_since=time.monotonic()-200)
    observations = []
    def observe(*args):
        observations.append(True)
        if len(observations) == 2:
            policy.write({**config, 'enabled': False}, policy.revision(policy.read()))
        return failed()
    monkeypatch.setattr(recovery, 'observe', observe)
    monkeypatch.setattr(recovery, 'perform', lambda *args: pytest.fail('disabled policy must not act'))
    assert monitor.tick()['phase'] == 'paused'
    assert not monitor.state.get('actions')


@pytest.mark.parametrize('stage', [0, 1, 2])
@pytest.mark.parametrize('change,phase', [
    ({'gateway_reachable': True}, 'upstream_unreachable'),
    ({'reachable': True, 'gateway_reachable': True}, 'healthy'),
    ({'interface': 'eth0', 'kind': 'ethernet'}, 'paused'),
    ({'connection_uuid': '11234567-89ab-cdef-0123-456789abcdef'}, 'paused'),
    ({'managed': False}, 'waiting_interface'),
    ({'kind': 'ethernet', 'carrier': False}, 'link_down'),
])
def test_full_preflight_cancels_stale_action_without_consuming_limits(files, monkeypatch, stage, change, phase):
    from rk3588_runtime import reboot_coordinator
    import time
    config = settings(allow_reboot=True)
    policy.write(config, policy.revision(policy.read()))
    monitor = recovery.Monitor()
    monitor.state = healthy(config)
    monitor.state.update(boot_id=monitor.boot_id, stage=stage, fault_since=time.monotonic()-2000)
    observations = iter([failed(), {**failed(), **change}])
    calls = []
    monkeypatch.setattr(recovery, 'observe', lambda *args: calls.append('observe') or next(observations))
    monkeypatch.setattr(recovery, 'perform', lambda *args: pytest.fail('stale action must be cancelled'))
    monkeypatch.setattr(reboot_coordinator, '_submit', lambda *args: pytest.fail('stale reboot must be cancelled'))
    assert monitor.tick()['phase'] == phase
    assert calls == ['observe', 'observe']
    assert monitor.state['actions'] == [] and monitor.state['reboots'] == []
    assert not reboot_coordinator.STATE_FILE.exists()


def test_valid_reboot_uses_coordinator_and_persists_both_limits_before_command(files, monkeypatch):
    from rk3588_runtime import reboot_coordinator, recovery_history
    import time
    config = settings(allow_reboot=True)
    policy.write(config, policy.revision(policy.read()))
    monitor = recovery.Monitor()
    monitor.state = healthy(config)
    monitor.state.update(boot_id=monitor.boot_id, stage=2, fault_since=time.monotonic()-2000)
    monkeypatch.setattr(recovery, 'observe', lambda *args: failed())
    calls = []
    def submit(force):
        assert not force
        assert len(json.loads(recovery.STATE_FILE.read_text())['reboots']) == 1
        shared = json.loads(reboot_coordinator.STATE_FILE.read_text())
        assert len(shared['attempts']) == 1 and shared['pending']['event_id']
        calls.append('reboot')
    monkeypatch.setattr(reboot_coordinator, '_submit', submit)
    assert monitor.tick()['phase'] == 'reboot_requested'
    assert calls == ['reboot']
    assert len(recovery_history.snapshot('network')['events']) == 1


def test_preflight_failure_never_persists_a_speculative_attempt(files, monkeypatch):
    import time
    config = settings()
    policy.write(config, policy.revision(policy.read()))
    monitor = recovery.Monitor()
    monitor.state = healthy(config)
    monitor.state.update(boot_id=monitor.boot_id, fault_since=time.monotonic()-2000)
    def observe(*args):
        if observe.called:
            raise RuntimeError('route query failed')
        observe.called = True
        return failed()
    observe.called = False
    monkeypatch.setattr(recovery, 'observe', observe)
    assert monitor.tick()['phase'] == 'error'
    assert not recovery.STATE_FILE.exists()
    assert not monitor.state['actions']


def test_pending_reboot_pauses_network_repair_without_consuming_limits(files, monkeypatch):
    from rk3588_runtime import reboot_coordinator
    import time
    monkeypatch.setattr(reboot_coordinator, '_submit', lambda *_: None)
    with reboot_coordinator.guard() as coordinator:
        coordinator.request('scheduled', 'scheduled reboot')
    config = settings()
    policy.write(config, policy.revision(policy.read()))
    monitor = recovery.Monitor()
    monitor.state = healthy(config)
    monitor.state.update(boot_id=monitor.boot_id, fault_since=time.monotonic()-2000)
    monkeypatch.setattr(recovery, 'observe', lambda *args: failed())
    monkeypatch.setattr(recovery, 'perform', lambda *args: pytest.fail('must not repair during reboot'))
    assert monitor.tick()['phase'] == 'reboot_pending'
    assert monitor.state['actions'] == [] and monitor.state['reboots'] == []


def test_malformed_limit_state_is_not_silently_reset(files):
    recovery.STATE_FILE.write_text('{broken')
    with pytest.raises(ValueError):
        recovery.Monitor()


def test_probe_is_bound_to_interface_and_no_payload_is_sent():
    server = socket.socket()
    server.bind(('127.0.0.1', 0)); server.listen(1); server.settimeout(5)
    received = []
    def accept():
        conn, _ = server.accept()
        with conn:
            conn.settimeout(3)
            received.append(conn.recv(20))
    thread = threading.Thread(target=accept)
    thread.start()
    try:
        assert recovery.tcp_probe('127.0.0.1', server.getsockname()[1], 'lo')
        thread.join(timeout=4)
        assert received == [b'']
    finally:
        server.close()


def test_probe_process_has_overall_timeout_and_never_uses_shell(monkeypatch):
    calls = []
    def run(command, timeout):
        calls.append((command, timeout))
        raise subprocess.TimeoutExpired(command, timeout)
    monkeypatch.setattr(recovery, 'run', run)
    assert not recovery.probe({'host': 'example.com', 'port': 443}, 'wlan0')['reachable']
    assert calls[0][1] == 6 and calls[0][0][-3:] == ['example.com', '443', 'wlan0']


@pytest.mark.parametrize('outcome', ['success', 'no_reply', 'timeout', 'missing_command'])
def test_ping_probe_has_no_port_and_is_bounded_and_bound_to_interface(monkeypatch, outcome):
    calls = []
    def run(command, timeout):
        calls.append((command, timeout))
        if outcome == 'timeout':
            raise subprocess.TimeoutExpired(command, timeout)
        if outcome == 'missing_command':
            raise FileNotFoundError('ping')
        return subprocess.CompletedProcess(command, 0 if outcome == 'success' else 1, '', '')
    monkeypatch.setattr(recovery, 'run', run)
    target = {'host': '192.0.2.20', 'method': 'ping'}
    assert recovery.probe(target, 'wlan0') == {**target, 'reachable': outcome == 'success'}
    assert calls == [(['ping', '-n', '-I', 'wlan0', '-c', '1', '-W', '1', '192.0.2.20'], 3)]


def test_ping_probe_can_reach_an_address_without_a_tcp_service():
    import shutil
    if not shutil.which('ping'):
        pytest.skip('ping is not installed')
    # Probe primitive only; loopback is intentionally forbidden as a saved target.
    assert recovery.probe({'host': '127.0.0.1', 'method': 'ping'}, 'lo')['reachable']


def test_ping_baseline_and_loss_use_the_same_recovery_limits():
    config = settings(targets=[{'host': '192.0.2.20', 'method': 'ping'}])
    state = healthy(config)
    for now, expected in ((300, None), (480, 'reconnect'), (780, 'restart_manager'), (1200, None)):
        state, _, action = recovery.evaluate(config, failed(), state, now, 10000 + now, 'boot')
        assert action == expected
    assert len(state['actions']) == 2 and not state['reboots']


def test_disabled_monitor_does_not_probe_or_require_network_tools(monkeypatch):
    def unexpected(*args, **kwargs):
        pytest.fail('disabled policy must not run network commands')
    monkeypatch.setattr(recovery, 'run', unexpected)
    monkeypatch.setattr(recovery, 'ip_json', unexpected)
    assert not recovery.observe(settings(enabled=False), {})['targets']


def test_auto_selection_tracks_default_route_and_uses_cached_scope_if_route_lost(monkeypatch):
    routes = [{'dev': 'wlan0', 'gateway': '192.0.2.1', 'metric': 600},
              {'dev': 'eth0', 'gateway': '192.0.2.1', 'metric': 100}]
    routes6 = [{'dev': 'wlan0', 'gateway': 'fe80::1', 'metric': 1}]
    monkeypatch.setattr(recovery, 'interfaces', lambda: [
        {'name': name, 'type': kind, 'state': 'connected'} for name, kind in [('wlan0', 'wifi'), ('eth0', 'ethernet')]])
    addresses = [{'ifname': name, 'flags': ['LOWER_UP'],
                  'addr_info': [{'scope': 'global', 'local': '192.0.2.2', 'prefixlen': 24}]} for name in ('wlan0', 'eth0')]
    def ip_json(*args):
        return addresses if args[0] == 'address' else routes if args[0] == 'route' else routes6
    monkeypatch.setattr(recovery, 'ip_json', ip_json)
    monkeypatch.setattr(recovery, 'run', lambda command, **kw: subprocess.CompletedProcess(command, 0, CONNECTION, ''))
    monkeypatch.setattr(recovery, 'probe', lambda target, interface: {**target, 'reachable': target['port'] == 443})
    config = settings(targets=[{'host': '203.0.113.10', 'port': 22}, {'host': '203.0.113.11', 'port': 443}])
    first = recovery.observe(config, {})
    assert first['interface'] == 'eth0' and first['reachable']
    routes[:] = routes[:1]
    assert recovery.observe(config, first)['interface'] == 'wlan0'
    routes.clear()
    routes6.clear()
    last = recovery.observe(config, first)
    assert last['interface'] == 'eth0' and not last['has_route']


def test_reconnect_uses_existing_uuid_on_selected_interface_and_never_creates_profile(monkeypatch):
    commands = []
    monkeypatch.setattr(recovery, 'run', lambda command, **kw:
                        commands.append(command) or subprocess.CompletedProcess(command, 0, '', ''))
    recovery.perform('reconnect', {'interface': 'eth0', 'connection_uuid': CONNECTION})
    assert commands == [['nmcli', '--wait', '15', 'connection', 'up', 'uuid', CONNECTION, 'ifname', 'eth0']]
    with pytest.raises(RuntimeError):
        recovery.perform('reconnect', {'interface': 'eth0', 'connection_uuid': ''})
    assert len(commands) == 1


def test_only_one_daemon_or_cli_check_can_operate_at_a_time(files, monkeypatch):
    monkeypatch.setattr(sys, 'argv', ['network_recovery', '--once'])
    with (recovery.STATE_FILE.parent / '.monitor.lock').open('a+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with pytest.raises(SystemExit, match='已运行'):
            recovery.main()
