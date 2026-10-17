import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('recovery_monitor', Path(__file__).parents[1] / 'recovery_monitor.py')
monitor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(monitor)


def test_transient_io_wait_does_not_reboot():
    blocked = {'100:123:unit': 'rk3588-app-demo.service'}
    state, reboot = monitor.evaluate(blocked, {}, 0, 10000, 'boot1')
    assert not reboot
    state, reboot = monitor.evaluate(blocked, state, 179, 10179, 'boot1')
    assert not reboot
    state, reboot = monitor.evaluate({}, state, 180, 10180, 'boot1')
    assert not reboot and not state['blocked']


def test_persistent_kernel_block_escalates_with_reboot_loop_limits():
    blocked = {'100:123:unit': 'rk3588-app-demo.service'}
    state, _ = monitor.evaluate(blocked, {}, 0, 10000, 'boot1')
    state, reboot = monitor.evaluate(blocked, state, 180, 10180, 'boot1')
    assert reboot and state['reboots'] == [10180]
    state, reboot = monitor.evaluate(blocked, state, 360, 10360, 'boot1')
    assert not reboot  # same boot and across boots both observe the cooldown
    state, reboot = monitor.evaluate(blocked, state, 0, 10400, 'boot2')
    assert not reboot and list(state['blocked'].values()) == [0]
    _, reboot = monitor.evaluate(blocked, state, 180, 10580, 'boot2')
    assert not reboot


def test_daily_limit_and_backwards_clock_never_create_reboot_loop():
    blocked = {'100:123:unit': 'rk3588-app-demo.service'}
    base = {'boot_id': 'b', 'blocked': {'100:123:unit': 0}, 'reboots': [10000, 12000, 14000]}
    assert not monitor.evaluate(blocked, base, 180, 20000, 'b')[1]
    assert not monitor.evaluate(blocked, base, 180, 9000, 'b')[1]
    assert monitor.evaluate(blocked, base, 180, 110000, 'b')[1]


def test_only_framework_d_state_processes_are_considered(tmp_path):
    for pid, status, unit in [(100, 'D', 'rk3588-app-demo.service'),
                              (101, 'S', 'rk3588-app-demo.service'),
                              (102, 'D', 'unrelated.service')]:
        process = tmp_path / str(pid)
        process.mkdir()
        # Field 22 (starttime) follows 19 fields after the closing comm parenthesis.
        (process / 'stat').write_text(f'{pid} (a process) {status} ' + '0 ' * 18 + '123\n')
        (process / 'cgroup').write_text(f'0::/system.slice/{unit}\n')
    assert monitor.blocked_processes(tmp_path) == {'100:100:123:rk3588-app-demo.service': 'rk3588-app-demo.service'}


def test_blocked_inference_thread_is_detected_with_sleeping_main(tmp_path):
    process = tmp_path / '200'
    thread = process / 'task/201'
    thread.mkdir(parents=True)
    (process / 'stat').write_text('200 (main) S ' + '0 ' * 18 + '123\n')
    (thread / 'stat').write_text('201 (infer worker) D ' + '0 ' * 18 + '456\n')
    (process / 'cgroup').write_text('1:name=systemd:/system.slice/rk3588-app-demo.service\n')
    assert monitor.blocked_processes(tmp_path) == {'200:201:456:rk3588-app-demo.service': 'rk3588-app-demo.service'}


def test_check_persists_reboot_attempt_before_command(tmp_path, monkeypatch):
    path = tmp_path / 'recovery/state.json'
    path.parent.mkdir()
    boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
    import json
    import time
    path.write_text(json.dumps({'boot_id': boot, 'blocked': {'100:123:unit': time.monotonic() - 200}, 'reboots': []}))
    monkeypatch.setattr(monitor, 'STATE_FILE', path)
    monkeypatch.setattr(monitor, 'current_policy', lambda: {
        'kernel_reboot_enabled': True, 'kernel_hang_sec': 180,
        'reboot_cooldown_sec': 1800, 'max_reboots_per_day': 3,
    })
    monkeypatch.setattr(monitor, 'recovery_failed', lambda *args: True)
    monkeypatch.setattr(monitor, 'blocked_processes', lambda: {'100:123:unit': 'rk3588-app-demo.service'})
    calls = []
    from rk3588_runtime import reboot_coordinator, recovery_policy
    monkeypatch.setattr(recovery_policy, 'read', lambda: {**recovery_policy.DEFAULTS, 'kernel_reboot_enabled': True})
    def submit(force):
        assert len(json.loads(path.read_text())['reboots']) == 1
        assert len(json.loads(reboot_coordinator.STATE_FILE.read_text())['attempts']) == 1
        calls.append(force)
    monkeypatch.setattr(reboot_coordinator, '_submit', submit)
    monitor.check()
    monitor.check()
    assert calls == [False]  # A repeated check merges; the first request is normal.


def test_d_state_alone_is_not_permission_to_reboot():
    blocked = {'pid': 'rk3588-app-demo.service'}
    state = {'boot_id': 'boot', 'blocked': {'pid': 0}, 'reboots': []}
    policy = {'kernel_reboot_enabled': True, 'kernel_hang_sec': 180,
              'reboot_cooldown_sec': 1800, 'max_reboots_per_day': 3}
    assert not monitor.evaluate(blocked, state, 200, 10000, 'boot', policy, set())[1]
    policy['kernel_reboot_enabled'] = False
    assert not monitor.evaluate(blocked, state, 200, 10000, 'boot', policy, set(blocked.values()))[1]
