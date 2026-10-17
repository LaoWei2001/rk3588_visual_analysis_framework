"""Device runtime supervisor. Launched by systemd, independent of the Web app."""
import argparse
import json
import os
from pathlib import Path
import signal
import threading
import time

from . import background_services, process_manager as pm, recovery_policy, runtime_state, recovery_history
from .service_watchdog import notify

STATUS_FILE = Path(os.environ.get('RUNTIME_STATUS_FILE', '/run/rk3588-runtime/status.json'))
EVENTS_FILE = Path(os.environ.get('RUNTIME_EVENTS_FILE', '/var/lib/rk3588-runtime/events.json'))


def read_json(path, default):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return default


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False) + '\n')
    temporary.replace(path)


class Manager:
    def __init__(self):
        self.boot_id = pm._current_boot_id()
        previous = read_json(STATUS_FILE, {})
        self.started_targets = set()
        self.observed = None
        if isinstance(previous, dict) and previous.get('boot_id') == self.boot_id:
            targets = previous.get('started_targets', [])
            if isinstance(targets, list):
                self.started_targets = {value for value in targets if isinstance(value, str)}
            self.observed = previous.get('observed')
        self.last_key = None
        self.next_attempt = 0
        self.failures = 0
        self.last_event = None
        self.last_error = None
        old_events = read_json(EVENTS_FILE, [])
        if isinstance(old_events, list) and len(old_events) > 20:
            atomic_json(EVENTS_FILE, old_events[-20:])

    def event(self, kind, message, *, fault_recovery=None):
        message = str(message)[:1024]
        signature = (kind, message)
        if signature == self.last_event and kind != 'restored':
            return
        self.last_event = signature
        print(f'[Runtime] {kind}: {message}', flush=True)
        events = read_json(EVENTS_FILE, [])
        if not isinstance(events, list):
            events = []
        events.append({'time': time.time(), 'kind': kind, 'message': message})
        atomic_json(EVENTS_FILE, events[-20:])
        if kind in ('restored', 'restore_error', 'service_error'):
            recovery_history.safe_record('application', kind,
                '目标程序不在运行，按自启意图或恢复配置启动' if kind == 'restored' else message,
                '运行协调服务启动目标程序或协调依赖服务',
                'performed' if kind == 'restored' else 'failed', message, source='独立运行协调服务',
                fault_recovery=fault_recovery)

    def tick(self):
        status = {'boot_id': self.boot_id, 'pid': os.getpid(), 'updated_at': time.time(),
                  'phase': 'running', 'target': None, 'current': None, 'error': self.last_error, 'retry_in_sec': 0}
        try:
            policy = recovery_policy.read()
            status['policy_revision'] = recovery_policy.revision(policy)
            if not policy['manager_enabled']:
                status['phase'] = 'disabled'
                self.last_error = status['error'] = None
            else:
                with pm.runtime_lock():
                    # Recover existing services even if the Web app has never started.
                    pm.recover_processes()
                    target = runtime_state.get_vision_boot_target()
                    current = pm.get_running_app_context()
                    status['target'] = target
                    target_key = json.dumps(target, sort_keys=True) if target else None
                    change_key = (target_key, status['policy_revision'])
                    if change_key != self.last_key:
                        self.last_key = change_key
                        self.next_attempt = 0
                        self.failures = 0
                        self.last_error = status['error'] = None
                    if current and target and current['app'] == target['app']:
                        self.started_targets.add(target_key)
                    if current is None and target:
                        allowed = policy['app_restart_enabled'] or target_key not in self.started_targets
                        if allowed and time.monotonic() >= self.next_attempt:
                            recovering = target_key in self.started_targets or self.failures > 0
                            pm.start_app(target['app'], target['config'])
                            self.started_targets.add(target_key)
                            self.failures = 0
                            self.next_attempt = 0
                            self.last_error = status['error'] = None
                            self.event('restored', f"已恢复应用 {target['app']}（{target['config']}）", fault_recovery=recovering)
                            current = pm.get_running_app_context()
                        elif not allowed:
                            status['phase'] = 'stopped'
                        else:
                            status['phase'] = 'retrying'
                            status['retry_in_sec'] = max(0, int(self.next_attempt - time.monotonic()))
                    if current:
                        self.last_error = status['error'] = None
                        self.failures = 0
                        self.next_attempt = 0
                        live = pm.get_status(current['app'])
                        status['current'] = {'app': current['app'], **live}
                        count = live.get('restart_count', 0)
                        before = (self.observed.get('restart_count', 0)
                                  if isinstance(self.observed, dict) and self.observed.get('app') == current['app'] else 0)
                        if live.get('pid'):
                            if count > before:
                                self.event('application_restart',
                                           f"应用 {current['app']} 已由系统自动恢复（第 {count} 次，PID {live['pid']}）")
                            self.observed = {'app': current['app'], 'pid': live['pid'], 'restart_count': count}
                        result = background_services.sync_services_for_running_app()
                        if result['errors']:
                            status['error'] = '；'.join(result['errors'])
                            status['phase'] = 'degraded'
                            self.event('service_error', status['error'])
        except Exception as exc:
            status['phase'] = 'retrying'
            status['error'] = str(exc)
            self.last_error = str(exc)
            if time.monotonic() >= self.next_attempt:
                self.failures += 1
                delay = min(300, 5 * 2 ** min(self.failures - 1, 6))
                self.next_attempt = time.monotonic() + delay
                self.event('restore_error', str(exc))
            status['retry_in_sec'] = max(0, int(self.next_attempt - time.monotonic()))
        status['started_targets'] = sorted(self.started_targets)
        status['observed'] = self.observed
        atomic_json(STATUS_FILE, status)
        return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--once', action='store_true', help='执行一次运行组合协调并退出')
    args = parser.parse_args()
    manager = Manager()
    stopping = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stopping.set())
    signal.signal(signal.SIGINT, lambda *_: stopping.set())
    while True:
        manager.tick()
        notify('WATCHDOG=1')
        if args.once or stopping.wait(5):
            break


if __name__ == '__main__':
    main()
