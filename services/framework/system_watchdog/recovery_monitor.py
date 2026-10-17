#!/usr/bin/env python3
"""Independent escalation for framework processes stuck in kernel D state.

Ordinary process crashes belong to systemd Restart=. Camera disconnection is not
a board failure. Only a continuously uninterruptible framework process escalates
to a board reboot, with persistent limits against reboot loops.
"""
import json
import math
import os
from pathlib import Path
import re
import subprocess
import time

STATE_FILE = Path(os.environ.get('KERNEL_RECOVERY_STATE_FILE', '/var/lib/rk3588-recovery/state.json'))
UNIT = re.compile(r'rk3588-app-[A-Za-z0-9_.-]+\.service|rk3588-console\.service|rk3588-runtime-manager\.service|rk3588-network-recovery\.service')
HANG_SECONDS = 180
REBOOT_COOLDOWN = 1800
REBOOT_WINDOW = 86400
MAX_REBOOTS = 3


def blocked_processes(proc_root=Path('/proc')):
    blocked = {}
    for process in proc_root.iterdir():
        if not process.name.isdigit():
            continue
        try:
            units = [part for line in (process / 'cgroup').read_text().splitlines()
                     for part in line.split(':', 2)[-1].split('/') if UNIT.fullmatch(part)]
            if not units:
                continue
            tasks = list((process / 'task').iterdir()) if (process / 'task').is_dir() else [process]
            # A blocked NPU thread may coexist with a perfectly healthy sleeping main thread.
            for task in tasks:
                try:
                    fields = (task / 'stat').read_text().rsplit(')', 1)[1].split()
                    if fields[0] == 'D':
                        blocked[f'{process.name}:{task.name}:{fields[19]}:{units[0]}'] = units[0]
                except (OSError, IndexError):
                    continue
        except (OSError, IndexError):
            continue
    return blocked


def evaluate(blocked, state, monotonic, utc, boot_id, policy=None, eligible_units=None, *, coordinated=False):
    settings = policy or {'kernel_reboot_enabled': True, 'kernel_hang_sec': HANG_SECONDS,
                          'reboot_cooldown_sec': REBOOT_COOLDOWN, 'max_reboots_per_day': MAX_REBOOTS}
    old = state.get('blocked', {}) if state.get('boot_id') == boot_id else {}
    if not isinstance(old, dict):
        old = {}
    valid_time = lambda value: isinstance(value, (int, float)) and math.isfinite(value) and value >= 0
    tracked = {key: old[key] if valid_time(old.get(key)) and old[key] <= monotonic else monotonic
               for key in blocked}
    history = state.get('reboots', [])
    if not isinstance(history, list):
        history = []
    attempts = [stamp for stamp in history if valid_time(stamp) and stamp > utc - REBOOT_WINDOW]
    updated = {'boot_id': boot_id, 'blocked': tracked, 'reboots': attempts}
    overdue = [blocked[key] for key, start in tracked.items()
               if monotonic - start >= settings['kernel_hang_sec']
               and (eligible_units is None or blocked[key] in eligible_units)]
    if not settings['kernel_reboot_enabled']:
        updated['blocked'] = {}
        return updated, False
    if not overdue:
        return updated, False
    if coordinated:
        # The shared coordinator owns reservation, limits and force escalation.
        return updated, True
    # A backwards wall-clock jump also keeps the cooldown in place.
    if attempts and (utc - max(attempts) < settings['reboot_cooldown_sec']
                     or len(attempts) >= settings['max_reboots_per_day']):
        print('[Recovery] kernel-blocked service persists; board reboot suppressed by cooldown/limit', flush=True)
        return updated, False
    updated['reboots'].append(utc)
    print(f"[Recovery] continuously kernel-blocked for {settings['kernel_hang_sec']}s after failed service recovery: {sorted(set(overdue))}; requesting board reboot",
          flush=True)
    return updated, True


def recovery_failed(unit, blocked=None):
    result = subprocess.run(['systemctl', 'show', unit,
                             '--property=SubState,Result,NRestarts,ExecMainStartTimestampMonotonic'],
                            capture_output=True, text=True, timeout=5)
    if result.returncode:
        return False
    status = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
    if (status.get('SubState', '').startswith(('stop-', 'final-'))
            or status.get('Result') in ('watchdog', 'timeout')):
        return True
    if int(status.get('NRestarts', '0')) > 0 and blocked:
        started = int(status.get('ExecMainStartTimestampMonotonic', '0')) / 1_000_000
        ticks = os.sysconf('SC_CLK_TCK')
        # A D-state thread older than the replacement service survived a previous
        # termination attempt. An unrelated historic restart is not sufficient.
        return any(value == unit and int(key.split(':')[2]) / ticks < started - .1
                   for key, value in blocked.items())
    return False


def current_policy():
    import sys
    # Standalone source installation and packaged installation share the same library.
    for directory in (Path(__file__).resolve().parents[1] / 'runtime_manager/python',
                      Path('/usr/lib/vision-analysis-platform/python')):
        if (directory / 'rk3588_runtime').is_dir() and str(directory) not in sys.path:
            sys.path.insert(0, str(directory))
    from rk3588_runtime.recovery_policy import read
    return read()


def persist(state):
    STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    temporary = STATE_FILE.with_suffix('.tmp')
    with temporary.open('w') as output:
        output.write(json.dumps(state) + '\n')
        output.flush()
        os.fsync(output.fileno())
    temporary.replace(STATE_FILE)
    directory_fd = os.open(STATE_FILE.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def check():
    policy = current_policy()
    from rk3588_runtime import reboot_coordinator
    with reboot_coordinator.guard() as coordinator:
        if coordinator is None:
            return  # A network repair or another reboot decision holds the lock.
        try:
            state = json.loads(STATE_FILE.read_text())
            if not isinstance(state, dict):
                raise ValueError('内核阻塞恢复状态异常')
        except FileNotFoundError:
            state = {}
        previous = state
        blocked = blocked_processes() if policy['kernel_reboot_enabled'] else {}
        eligible = {unit for unit in set(blocked.values()) if recovery_failed(unit, blocked)}
        state, reboot = evaluate(blocked, state, time.monotonic(), time.time(),
                                 Path('/proc/sys/kernel/random/boot_id').read_text().strip(), policy, eligible,
                                 coordinated=True)
        if state != previous:
            persist(state)
        if reboot:
            def accepted(event_id, stamp):
                state['reboots'].append(stamp)
                persist(state)
            coordinator.request('kernel',
                f"框架服务持续内核阻塞超过 {policy['kernel_hang_sec']} 秒，服务恢复未成功",
                detail='受影响服务：' + '、'.join(sorted(eligible)), on_accept=accepted)


if __name__ == '__main__':
    check()
