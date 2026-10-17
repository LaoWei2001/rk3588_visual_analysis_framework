"""Serialize software reboots and network repairs; hardware watchdogs stay independent."""
import argparse
from contextlib import contextmanager
import fcntl
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import time

from . import network_policy, recovery_policy, recovery_history as audit

STATE_FILE = Path(os.environ.get('REBOOT_COORDINATOR_STATE_FILE', '/var/lib/rk3588-reboot/state.json'))
LEGACY_FILES = (Path(os.environ.get('NETWORK_RECOVERY_STATE_FILE', '/var/lib/rk3588-network-recovery/state.json')),
                Path(os.environ.get('KERNEL_RECOVERY_STATE_FILE', '/var/lib/rk3588-recovery/state.json')))
FORCE_AFTER_SEC = 120
PENDING_TIMEOUT_SEC = 300
SOURCES = {'network': ('network', '独立网络恢复服务'),
           'kernel': ('system', '独立内核阻塞恢复监测'),
           'scheduled': ('system', '每日定时重启')}


def _number(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def _read():
    try:
        if STATE_FILE.stat().st_size > 128 * 1024:
            raise ValueError('整机重启协调记录过大')
        state = json.loads(STATE_FILE.read_text())
    except FileNotFoundError:
        # Import previous attempts once; upgrading must not reset reboot limits.
        attempts = []
        for path in LEGACY_FILES:
            try:
                old = json.loads(path.read_text())
            except FileNotFoundError:
                continue
            stamps = old.get('reboots', []) if isinstance(old, dict) else None
            if not isinstance(stamps, list) or any(not _number(t) for t in stamps):
                raise ValueError('原整机重启限次记录异常')
            attempts.extend({'time': t, 'fault': True} for t in stamps if t > time.time() - 86400)
        state = {'version': 1, 'attempts': attempts, 'pending': None}
    if (not isinstance(state, dict) or state.get('version') != 1 or 'pending' not in state
            or not isinstance(state.get('attempts'), list)
            or any(not isinstance(a, dict) or not _number(a.get('time')) or type(a.get('fault')) is not bool
                   for a in state['attempts'])):
        raise ValueError('整机重启协调记录异常，停止自动操作')
    pending = state.get('pending')
    if pending is not None and (not isinstance(pending, dict)
            or not isinstance(pending.get('boot_id'), str) or not pending['boot_id']
            or not _number(pending.get('monotonic')) or type(pending.get('forced')) is not bool
            or not isinstance(pending.get('event_id'), str) or not pending['event_id']
            or not isinstance(pending.get('reasons'), dict)
            or not pending['reasons'] or set(pending['reasons']) - SOURCES.keys()
            or any(not isinstance(v, str) for v in pending['reasons'].values())):
        raise ValueError('待重启状态异常，停止自动操作')
    return state


def _write(state):
    temporary = STATE_FILE.with_suffix('.tmp')
    try:
        with temporary.open('w') as output:
            json.dump(state, output, ensure_ascii=False)
            output.write('\n'); output.flush(); os.fsync(output.fileno())
        os.replace(temporary, STATE_FILE)
        directory = os.open(STATE_FILE.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        temporary.unlink(missing_ok=True)


def _systemctl():
    path = os.environ.get('SYSTEMCTL_PATH') or shutil.which('systemctl')
    if not path or not Path(path).is_absolute():
        raise ValueError('systemctl 路径必须为绝对路径')
    return path


def _system_state():
    result = subprocess.run([_systemctl(), 'is-system-running'], capture_output=True, text=True, timeout=5)
    return result.stdout.strip()


def _submit(force=False):
    subprocess.run([_systemctl(), 'reboot'] + (['--force'] if force else []), check=True, timeout=15)


def _limits(source):
    system, network = recovery_policy.read(), network_policy.read()
    enabled = {'kernel': system['kernel_reboot_enabled'],
               'network': network['enabled'] and network['allow_reboot'], 'scheduled': True}
    policies = ([system] if enabled['kernel'] else []) + ([network] if enabled['network'] else [])
    policies = policies or [system]
    return (enabled[source], max(p['reboot_cooldown_sec'] for p in policies),
            min(p['max_reboots_per_day'] for p in policies))


@contextmanager
def guard():
    """Also held during a network repair, so scheduled reboots cannot interrupt it."""
    STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    with STATE_FILE.with_suffix('.lock').open('a+') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            yield None
            return
        try:
            yield Coordinator(_read())
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


class Coordinator:
    def __init__(self, state):
        self.state = state
        pending = state.get('pending')
        if pending and pending['boot_id'] != audit.boot_id():
            state['pending'] = None
            _write(state)  # A new boot clears only the in-progress flag, never the limits.

    @property
    def pending(self):
        pending = self.state.get('pending')
        if (pending and not pending['forced']
                and time.monotonic() - pending['monotonic'] >= PENDING_TIMEOUT_SEC
                and _system_state() in ('running', 'degraded')):
            audit.update(pending['event_id'], 'failed', '重启提交后 300 秒仍未进入关机流程，释放待重启状态；保留限次记录。')
            self.state['pending'] = pending = None
            _write(self.state)
        return pending

    def request(self, source, reason, *, detail='', evidence='', diagnosis=None, on_accept=None):
        if source not in SOURCES:
            raise ValueError('未知整机重启来源')
        enabled, cooldown, maximum = _limits(source)
        if not enabled:
            return {'status': 'disabled', 'message': '整机重启策略已关闭'}
        now, utc = time.monotonic(), time.time()
        # A fresh, still-blocked kernel check may escalate even if the normal
        # request is older than the ordinary pending-state expiry.
        pending = self.state.get('pending') if source == 'kernel' else self.pending
        if pending:
            changed = source not in pending['reasons']
            pending['reasons'].setdefault(source, str(reason)[:512])
            # Only a fresh kernel-blocked check may upgrade a timed-out normal request.
            force = source == 'kernel' and not pending['forced'] and now - pending['monotonic'] >= FORCE_AFTER_SEC
            if changed or force:
                text = '；'.join(f'{SOURCES[k][1]}：{v}' for k, v in pending['reasons'].items())
                audit.update(pending['event_id'], 'performed',
                             '合并重启原因：' + text + ('；正常重启超过 120 秒，确认内核阻塞仍在，已升级强制重启。' if force or pending['forced'] else '；沿用已提交请求，不重复重启或计数。'),
                             action='systemctl reboot --force（正常重启超时后升级）' if force else None)
                if force:
                    pending['forced'] = True
                _write(self.state)  # Even a daemon crash must not submit a second force request.
            if force:
                self._execute(pending, force=True)
            return {'status': 'escalated' if force else 'pending', 'event_id': pending['event_id'],
                    'message': '整机重启已在进行，已合并请求'}
        attempts = [a for a in self.state['attempts'] if a['time'] > utc - 86400]
        if (attempts and utc - max(a['time'] for a in attempts) < cooldown
                or source != 'scheduled' and sum(a['fault'] for a in attempts) >= maximum):
            self.state['attempts'] = attempts
            _write(self.state)  # Persist imported limits even when the first request is denied.
            return {'status': 'limited', 'message': '共享整机重启冷却或 24 小时故障重启次数已达限制'}
        if _system_state() == 'stopping':
            return {'status': 'stopping', 'message': '系统已进入关机流程，不再提交重启'}
        category, label = SOURCES[source]
        event_id = audit.record(category, 'board_reboot_request', reason, 'systemctl reboot 请求正常整机重启',
                                source=label, detail=detail, evidence=evidence, diagnosis=diagnosis,
                                fault_recovery=source != 'scheduled')
        pending = {'boot_id': audit.boot_id(), 'monotonic': now, 'event_id': event_id,
                   'forced': False, 'reasons': {source: str(reason)[:512]}, 'detail': str(detail)[:500]}
        self.state.update(attempts=attempts + [{'time': utc, 'fault': source != 'scheduled'}], pending=pending)
        _write(self.state)
        try:
            if on_accept:
                on_accept(event_id, utc)
        except Exception:
            self.state['pending'] = None
            _write(self.state)
            audit.update(event_id, 'failed', '来源恢复状态未能持久化，未执行重启；保留本次限次记录。')
            raise
        self._execute(pending)
        return {'status': 'submitted', 'event_id': event_id, 'message': '正常整机重启命令已提交'}

    def _execute(self, pending, force=False):
        try:
            _submit(force)
        except subprocess.TimeoutExpired:
            audit.update(pending['event_id'], 'unknown', '重启命令等待超时，提交结果未确认；保留待重启状态，防止重复提交。')
            raise
        except (OSError, subprocess.SubprocessError):
            if not force:
                self.state['pending'] = None
                _write(self.state)
            audit.update(pending['event_id'], 'failed', '重启命令失败；保留限次记录。')
            raise
        reasons = '；'.join(f'{SOURCES[k][1]}：{v}' for k, v in pending['reasons'].items())
        audit.update(pending['event_id'], 'performed',
                     ('正常重启超过 120 秒且确认内核阻塞仍在，强制重启命令已提交。' if force else '正常重启命令已提交，等待设备重新启动。')
                     + '；' + reasons + '；' + str(pending.get('detail', '')))


def scheduled_reboot():
    deadline = time.monotonic() + 60
    while True:
        with guard() as coordinator:
            if coordinator is not None:
                result = coordinator.request('scheduled', '到达每日定时重启时间')
                if result['status'] not in ('submitted', 'pending', 'escalated'):
                    audit.record('system', 'scheduled_reboot_skipped', '到达每日定时重启时间',
                                 '本次定时重启未执行', 'performed', result['message'],
                                 source='每日定时重启', fault_recovery=False)
                return result
        if time.monotonic() >= deadline:
            raise RuntimeError('恢复动作仍在进行，定时重启等待协调锁超时，未打断恢复')
        time.sleep(1)


def daily_service_content(systemctl='/usr/bin/systemctl'):
    if not Path(systemctl).is_absolute() or '\n' in systemctl or '\r' in systemctl:
        raise ValueError('systemctl 路径无效')
    escaped = systemctl.replace('\\', '\\\\').replace('"', '\\"').replace('%', '%%')
    return ('[Unit]\nDescription=RK3588 Daily Scheduled Reboot\n\n[Service]\nType=oneshot\n'
            'Environment=PYTHONPATH=/usr/lib/vision-analysis-platform/python\n'
            f'Environment="SYSTEMCTL_PATH={escaped}"\n'
            'ExecStart=/usr/bin/python3 -m rk3588_runtime.reboot_coordinator --scheduled\n'
            'TimeoutStartSec=90\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scheduled', required=True, action='store_true')
    parser.parse_args()
    print(json.dumps(scheduled_reboot(), ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
