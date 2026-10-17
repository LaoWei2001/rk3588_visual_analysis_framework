"""Bounded network probes and staged recovery, without Web dependencies."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager, nullcontext
import copy
import fcntl
import ipaddress
import json
import math
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import threading
import time

from . import network_policy as policy
from . import recovery_history as audit
from . import reboot_coordinator
from .data_dir import APPS_ROOT
from .service_watchdog import notify

UNIT = 'rk3588-network-recovery.service'
STATE_FILE = Path(os.environ.get('NETWORK_RECOVERY_STATE_FILE', '/var/lib/rk3588-network-recovery/state.json'))
STATUS_FILE = Path(os.environ.get('NETWORK_RECOVERY_STATUS_FILE', '/run/rk3588-network-recovery/status.json'))
TRANSACTION_DIR = Path(os.environ.get('NETWORK_TRANSACTION_DIR', str(APPS_ROOT / '.network_transactions')))
UUID = re.compile(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}')
ACTIVE_TRANSACTIONS = {'scheduled', 'activating', 'awaiting_confirmation', 'committing', 'rolling_back'}


def run(command, timeout=5):
    return subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                          env={**os.environ, 'LC_ALL': 'C'})


def atomic_json(path, value, durable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.tmp')
    try:
        with temporary.open('w') as output:
            json.dump(value, output, ensure_ascii=False, indent=2)
            output.write('\n'); output.flush()
            if durable:
                os.fsync(output.fileno())
        os.replace(temporary, path)
        if durable:
            fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
    finally:
        temporary.unlink(missing_ok=True)


def interfaces():
    result = run(['nmcli', '-t', '-f', 'DEVICE,TYPE,STATE', 'device', 'status'])
    if result.returncode:
        raise RuntimeError('NetworkManager 未响应或不可用')
    entries = []
    for line in result.stdout.splitlines():
        fields = line.split(':', 2)
        if (len(fields) == 3 and policy.INTERFACE.fullmatch(fields[0])
                and fields[1] in ('wifi', 'ethernet')):
            entries.append({'name': fields[0], 'type': fields[1], 'state': fields[2]})
    return entries


def tcp_probe(host, port, interface):
    """Executed in a disposable subprocess so DNS cannot block the supervisor."""
    deadline = time.monotonic() + 4
    addresses = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    for family, kind, protocol, _, address in addresses[:8]:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        with socket.socket(family, kind, protocol) as sock:
            sock.settimeout(min(2, remaining))
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, interface.encode() + b'\0')
            try:
                sock.connect(address)
                return True
            except OSError:
                continue
    return False


def probe(target, interface):
    try:
        if target.get('method') == 'ping':
            result = run(['ping', '-n', '-I', interface, '-c', '1', '-W', '1', target['host']], timeout=3)
        else:
            result = run([sys.executable, '-m', 'rk3588_runtime.network_recovery', '--probe',
                          target['host'], str(target['port']), interface], timeout=6)
        return {**target, 'reachable': result.returncode == 0}
    except (OSError, subprocess.SubprocessError):
        return {**target, 'reachable': False}


def ip_json(*args):
    result = run(['ip', '-j', *args], timeout=3)
    if result.returncode:
        raise RuntimeError('无法读取本机网络地址或路由')
    data = json.loads(result.stdout)
    if not isinstance(data, list):
        raise ValueError('网络命令返回格式异常')
    return data


def observe(settings, previous):
    if not settings['enabled']:
        return {'interface': previous.get('interface', ''), 'kind': previous.get('kind', ''),
                'manager_ok': True, 'managed': True, 'carrier': False, 'present': False,
                'addresses': [], 'has_route': False, 'gateway': '', 'gateway_reachable': None,
                'connection_uuid': '', 'targets': [], 'reachable': False}
    try:
        inventory = interfaces()
        manager_ok = True
    except (OSError, RuntimeError, subprocess.SubprocessError):
        inventory, manager_ok = [], False
    addresses = ip_json('address', 'show')
    routes4 = ip_json('route', 'show', 'default')
    routes6 = ip_json('-6', 'route', 'show', 'default')
    kinds = {item['name']: item['type'] for item in inventory}
    if previous.get('interface') and previous.get('kind') in ('wifi', 'ethernet'):
        kinds.setdefault(previous['interface'], previous['kind'])
    unmanaged = {item['name'] for item in inventory if item['state'] == 'unmanaged'}
    # Route metrics from different address families are not comparable. Prefer
    # IPv4 defaults; IPv6-only deployments can also select an interface explicitly.
    routes = [r for group in (routes4, routes6)
              for r in sorted(group, key=lambda r: r.get('metric', 0))
              if r.get('dev') in kinds and r.get('dev') not in unmanaged]
    interface = settings['interface'] or (routes[0]['dev'] if routes else previous.get('interface', ''))
    entry = next((a for a in addresses if a.get('ifname') == interface), {})
    route = next((r for r in routes if r.get('dev') == interface), {})
    usable = [a for a in entry.get('addr_info', []) if a.get('scope') == 'global'
              and not set(a.get('flags', [])) & {'tentative', 'dadfailed'}]
    local_route = False
    for target in settings['targets']:
        try:
            target_ip = ipaddress.ip_address(target['host'])
            local_route |= any(target_ip in ipaddress.ip_network(f"{a['local']}/{a['prefixlen']}", strict=False)
                               for a in usable if (':' in a['local']) == (target_ip.version == 6))
        except ValueError:
            pass
    observation = {'interface': interface, 'kind': kinds.get(interface, ''), 'manager_ok': manager_ok,
                   'managed': not any(i['name'] == interface and i['state'] == 'unmanaged' for i in inventory),
                   'carrier': 'LOWER_UP' in entry.get('flags', []), 'present': bool(entry),
                   'addresses': [a['local'] for a in usable], 'has_route': bool(route) or local_route,
                   'gateway': route.get('gateway', ''), 'gateway_reachable': None,
                   'connection_uuid': '', 'targets': [], 'reachable': False}
    if interface and manager_ok:
        result = run(['nmcli', '-g', 'GENERAL.CON-UUID', 'device', 'show', interface])
        candidate = result.stdout.strip()
        if result.returncode == 0 and UUID.fullmatch(candidate):
            observation['connection_uuid'] = candidate
    if settings['enabled'] and interface and observation['kind'] in ('wifi', 'ethernet') and entry and observation['managed']:
        with ThreadPoolExecutor(max_workers=4) as executor:
            observation['targets'] = list(executor.map(lambda t: probe(t, interface), settings['targets']))
        observation['reachable'] = any(t['reachable'] for t in observation['targets'])
        gateway = observation['gateway']
        if gateway:
            try:
                result = run(['ping', '-n', '-I', interface, '-c', '1', '-W', '1', gateway], timeout=3)
                observation['gateway_reachable'] = result.returncode == 0
            except (OSError, subprocess.SubprocessError):
                pass
    return observation


@contextmanager
def network_guard():
    """Share the Web network transaction lock, never import the Web module."""
    TRANSACTION_DIR.mkdir(parents=True, exist_ok=True)
    with (TRANSACTION_DIR / '.lock').open('a+') as handle:
        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            yield False
            return
        try:
            safe = True
            for path in TRANSACTION_DIR.glob('*.json'):
                try:
                    value = json.loads(path.read_text())
                    if not isinstance(value, dict) or value.get('status') in ACTIVE_TRANSACTIONS:
                        safe = False
                except (OSError, ValueError):
                    safe = False
            yield safe
        finally:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def history(values, utc, window):
    if not isinstance(values, list) or any(type(v) not in (int, float) or not math.isfinite(v) or v < 0 for v in values):
        raise ValueError('网络恢复限频记录异常，已停止自动操作')
    # Future timestamps are retained to fail closed across backwards clock jumps.
    return [v for v in values if v > utc - window]


def evaluate(settings, observation, old, monotonic, utc, boot_id):
    state = copy.deepcopy(old)
    fingerprint = policy.revision(settings)
    if state.get('boot_id') != boot_id:
        state.update(boot_id=boot_id, fault_since=None, stage=0, next_action_at=0)
    if state.get('policy_revision') != fingerprint or state.get('interface') != observation['interface']:
        state.update(policy_revision=fingerprint, armed=False, fault_since=None, stage=0, next_action_at=0,
                     known_gateway='', connection_uuid='')
    state.update(interface=observation['interface'], kind=observation['kind'])
    if observation.get('connection_uuid'):
        state['connection_uuid'] = observation['connection_uuid']
    state['actions'] = history(state.get('actions', []), utc, 3600)
    state['reboots'] = history(state.get('reboots', []), utc, 86400)
    status = {'phase': 'healthy', 'message': '检测目标可达', 'fault_age_sec': 0, 'local_fault': False}
    def finish(phase, message, action=None, reset=False):
        status.update(phase=phase, message=message)
        if reset:
            state.update(fault_since=None, stage=0, next_action_at=0)
        return state, status, action
    if not settings['enabled']:
        state['armed'] = False
        return finish('disabled', '网络自动恢复未开启', reset=True)
    if not observation['interface'] or observation['kind'] not in ('wifi', 'ethernet') or not observation.get('managed', True):
        return finish('waiting_interface', '等待可识别的有线或 Wi-Fi 管理网卡', reset=True)
    if observation['reachable']:
        state['armed'] = True
        if observation.get('gateway_reachable'):
            state['known_gateway'] = observation['gateway']
        return finish('healthy', '至少一个网络检测目标可达', reset=True)
    if monotonic < 120:
        return finish('boot_grace', '开机后前 120 秒等待网络就绪', reset=True)
    if not state.get('armed'):
        return finish('waiting_baseline', '尚无成功探测记录；请确认目标及检测方式，暂不自动恢复', reset=True)
    if observation['kind'] == 'ethernet' and observation['present'] and not observation['carrier']:
        return finish('link_down', '有线链路无信号，等待接线或交换机恢复，不因此重启', reset=True)
    local_fault = (not observation['manager_ok'] or not observation['addresses'] or not observation['has_route']
                   or bool(observation.get('gateway') == state.get('known_gateway')
                           and observation.get('gateway') and observation.get('gateway_reachable') is False))
    status['local_fault'] = local_fault
    if observation.get('gateway_reachable') and not local_fault:
        return finish('upstream_unreachable', '网关可达但检测目标均不可达，暂按上游异常处理', reset=True)
    if state.get('fault_since') is None or state['fault_since'] > monotonic:
        state['fault_since'] = monotonic
    age = monotonic - state['fault_since']
    status['fault_age_sec'] = int(age)
    if age < settings['failure_timeout_sec']:
        return finish('suspected', '探测持续失败，等待异常确认期限')
    if monotonic < state.get('next_action_at', 0):
        return finish('recovering', '已尝试恢复，等待网络重新建立')
    stage = state.get('stage', 0)
    if stage == 1 and not settings['allow_manager_restart']:
        state['stage'] = stage = 2
    if stage >= 2:
        if settings['allow_reboot'] and local_fault and age >= settings['reboot_after_sec']:
            attempts = state['reboots']
            if len(attempts) >= settings['max_reboots_per_day'] or (attempts and utc - max(attempts) < settings['reboot_cooldown_sec']):
                return finish('rate_limited', '已达到整机重启冷却时间或 24 小时次数限制')
            state['reboots'].append(utc)
            return finish('reboot_requested', '持续失联且存在本机异常，已记录整机重启请求', 'reboot')
        # Keep trying in a bounded new cycle; do not stop recovery forever after
        # one unsuccessful cycle, or clear persistent limits across service restarts.
        if state['actions'] and utc - max(state['actions']) >= max(900, settings['action_cooldown_sec']):
            state['stage'] = stage = 0
        else:
            return finish('recovery_exhausted', '本轮恢复未成功，等待重试；整机重启需另行开启且存在本机异常')
    if len(state['actions']) >= 6 or (state['actions'] and utc - max(state['actions']) < settings['action_cooldown_sec']):
        return finish('rate_limited', '网络恢复操作冷却中，最多每小时 6 次')
    action = 'reconnect' if stage == 0 else 'restart_manager'
    state['actions'].append(utc)
    state['stage'] = stage + 1
    state['next_action_at'] = monotonic + settings['recovery_wait_sec']
    return finish('recovering', '重新激活原连接' if stage == 0 else '重启 NetworkManager 网络服务', action)


def perform(action, state):
    interface = state['interface']
    if not policy.INTERFACE.fullmatch(interface):
        raise ValueError('恢复网卡名称不正确')
    if action == 'reconnect':
        connection = state.get('connection_uuid', '')
        if not UUID.fullmatch(connection):
            raise RuntimeError('没有已知连接配置，跳过连接激活；不会创建或改写网络配置')
        command = ['nmcli', '--wait', '15', 'connection', 'up', 'uuid', connection, 'ifname', interface]
    elif action == 'restart_manager':
        command = ['systemctl', 'restart', 'NetworkManager.service']
    elif action == 'reboot':
        raise ValueError('整机重启必须经过统一协调入口')
    else:
        raise ValueError('不支持的网络恢复操作')
    result = run(command, timeout=20)
    if result.returncode:
        raise RuntimeError((result.stderr or result.stdout or '网络恢复操作失败').strip()[-500:])


def describe_fault(observation, state, result):
    faults = []
    if not observation.get('present', True):
        faults.append('网卡未出现，可能为设备移除或驱动未就绪')
    elif not observation.get('carrier', True):
        faults.append('网口链路未建立' if observation.get('kind') == 'ethernet' else '无线链路未建立')
    if not observation['manager_ok']:
        faults.append('NetworkManager 未响应')
    if not observation['addresses']:
        faults.append('网卡没有有效 IP 地址')
    if not observation['has_route']:
        faults.append('网卡缺少路由')
    if observation.get('gateway_reachable') is False:
        faults.append('网关未响应 ICMP 探测（可能禁用 ICMP）')
    elif observation.get('gateway_reachable'):
        faults.append('网关可达但检测目标不可达，需检查上游网络或目标服务')
    reason = f"网卡 {state['interface']} 的检测目标持续不可达约 {result.get('fault_age_sec', 0)} 秒"
    if faults:
        reason += '；' + '、'.join(faults)
    return dict(reason=reason,
                evidence=f"网卡={state['interface']}；本机网络异常={result.get('local_fault', False)}；检测结果="
                + json.dumps(observation.get('targets', []), ensure_ascii=False)[:900]
                + f"；链路={observation.get('carrier')}；IP={observation.get('addresses', [])}；"
                f"路由={observation.get('has_route')}；网关={observation.get('gateway', '')}；"
                f"网关探测={observation.get('gateway_reachable')}；NetworkManager={observation.get('manager_ok')}",
                diagnosis={'confidence': 'confirmed', 'findings': [],
                           'limitations': ['确认检测目标不可达；物理线路、DNS、上游或目标内部根因需更多证据。']})


class Monitor:
    def __init__(self):
        try:
            self.state = json.loads(STATE_FILE.read_text())
        except FileNotFoundError:
            self.state = {}
        if not isinstance(self.state, dict):
            raise ValueError('网络恢复状态文件格式异常')
        if isinstance(self.state.get('events'), list) and len(self.state['events']) > 20:
            self.state['events'] = self.state['events'][-20:]
            atomic_json(STATE_FILE, self.state, durable=True)
        self.boot_id = Path('/proc/sys/kernel/random/boot_id').read_text().strip()

    def tick(self):
        now, utc = time.monotonic(), time.time()
        status = {'boot_id': self.boot_id, 'updated_at': utc, 'pid': os.getpid()}
        try:
            settings = policy.read()
            observation = observe(settings, self.state)
            status['observation'] = observation
            with network_guard() as safe, (reboot_coordinator.guard() if settings['enabled'] else nullcontext(None)) as coordinator:
                if not settings['enabled']:
                    state, result, action = evaluate(settings, observation, self.state, now, utc, self.boot_id)
                elif not safe:
                    state = {**self.state, 'fault_since': None, 'stage': 0, 'next_action_at': 0}
                    result = {'phase': 'paused', 'message': '网络配置事务正在进行或记录异常，暂停自动恢复'}
                    action = None
                elif coordinator is None:
                    state, result, action = copy.deepcopy(self.state), {'phase': 'paused', 'message': '其他恢复动作正在进行，下一轮重新检查'}, None
                elif policy.revision(settings) != policy.revision(policy.read()):
                    state, result, action = copy.deepcopy(self.state), {'phase': 'paused', 'message': '策略正在变更，下一轮重新检查'}, None
                else:
                    state, result, action = evaluate(settings, observation, self.state, now, utc, self.boot_id)
                if action:
                    # Discard the speculative decision (including its counters),
                    # then observe all local and remote evidence under both locks.
                    first = observation
                    observation = observe(settings, self.state)
                    status['observation'] = observation
                    state, result, action = evaluate(settings, observation, self.state, time.monotonic(), time.time(), self.boot_id)
                    if (first['interface'] != observation['interface']
                            or first.get('connection_uuid') != observation.get('connection_uuid')):
                        state.update(armed=False, fault_since=None, stage=0, next_action_at=0,
                                     actions=history(self.state.get('actions', []), utc, 3600),
                                     reboots=history(self.state.get('reboots', []), utc, 86400))
                        result, action = {'phase': 'paused', 'message': '管理网卡或连接已变化，取消原恢复动作，重新建立探测基线'}, None
                    elif action and action != 'reboot' and coordinator.pending:
                        state, result, action = copy.deepcopy(self.state), {'phase': 'reboot_pending', 'message': '整机重启已在进行，暂停网络修复'}, None
                if action and policy.revision(settings) != policy.revision(policy.read()):
                    state, result, action = copy.deepcopy(self.state), {'phase': 'paused', 'message': '策略已变更，取消尚未执行的恢复'}, None
                if action == 'reboot':
                    def accepted(event_id, stamp):
                        for previous_id in self.state.get('history_pending_ids', []):
                            audit.update(previous_id, 'failed', '等待期限内未恢复网络连通，升级整机重启。')
                        state['history_pending_ids'] = [event_id]
                        state['history_policy_revision'] = state.get('policy_revision')
                        state['reboots'][-1] = stamp
                        atomic_json(STATE_FILE, state, durable=True)
                        self.state = copy.deepcopy(state)
                    reply = coordinator.request('network', **describe_fault(observation, state, result), on_accept=accepted)
                    if reply['status'] != 'submitted':
                        state = copy.deepcopy(self.state)
                        result = {'phase': 'rate_limited' if reply['status'] == 'limited' else 'reboot_pending',
                                  'message': reply['message']}
                    action = None  # The coordinator alone submits reboot commands.
                previous_phase = self.state.get('last_phase')
                state['last_phase'] = result['phase']
                events = state.setdefault('events', [])
                if action or result['phase'] != previous_phase:
                    print(f"[NetworkRecovery] {action or result['phase']}: {result['message']}", flush=True)
                    events.append({'time': utc, 'kind': action or result['phase'], 'message': result['message']})
                    state['events'] = events[-20:]
                pending_ids = state.setdefault('history_pending_ids', [])
                if pending_ids and state.get('history_policy_revision') not in (None, state.get('policy_revision')):
                    for event_id in pending_ids:
                        audit.update(event_id, 'unknown', '恢复策略或检测目标已更改，原操作结果无法继续核实。')
                    state['history_pending_ids'] = pending_ids = []
                if result['phase'] == 'healthy':
                    for event_id in pending_ids:
                        audit.update(event_id, 'recovered', '恢复操作后至少一个配置的网络检测目标已可达。')
                    if not pending_ids and previous_phase in ('suspected', 'link_down', 'upstream_unreachable', 'waiting_interface', 'recovering', 'recovery_exhausted', 'rate_limited'):
                        audit.record('network', 'external_recovery', '此前网络检测异常，现已检测到目标可达',
                                     '未执行自动恢复操作，网络自行恢复或经外部操作恢复', 'recovered',
                                     '具体外部恢复方式未确认。', source='独立网络恢复服务')
                    state['history_pending_ids'] = []
                if action:
                    for event_id in pending_ids:
                        audit.update(event_id, 'failed', '等待期限内未恢复网络连通，继续尝试下一步恢复。')
                    method = {'reconnect': '重新激活原网络连接', 'restart_manager': '重启 NetworkManager 网络服务',
                              'reboot': '请求整机重启以恢复网络'}[action]
                    event_id = audit.record('network', 'board_reboot_request' if action == 'reboot' else action,
                                            action=method, source='独立网络恢复服务',
                                            detail='先保存恢复动作，再执行；命令成功不代表网络已经恢复。',
                                            **describe_fault(observation, state, result))
                    state['history_pending_ids'] = [event_id]
                    state['history_policy_revision'] = state.get('policy_revision')
                # Persist actions and reboot limits BEFORE attempting recovery.
                if state != self.state:
                    atomic_json(STATE_FILE, state, durable=True)
                self.state = state
                if action:
                    try:
                        perform(action, state)
                        audit.update(event_id, 'performed', '恢复命令已完成，等待网络探测确认。' if action != 'reboot' else '重启命令已提交，等待设备重新启动。')
                    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
                        audit.update(event_id, 'failed', str(exc))
                        state['history_pending_ids'] = []
                        result['error'] = str(exc)
                        print(f'[NetworkRecovery] action_error: {exc}', flush=True)
                        state['events'].append({'time': time.time(), 'kind': 'action_error', 'message': str(exc)[:1024]})
                        state['events'] = state['events'][-20:]
                        atomic_json(STATE_FILE, state, durable=True)
            status.update(result, policy_revision=policy.revision(settings), armed=bool(self.state.get('armed')))
        except Exception as exc:
            status.update(phase='error', message='网络恢复检查失败，暂不执行自动操作', error=str(exc))
        atomic_json(STATUS_FILE, status)
        return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--once', action='store_true', help='执行一轮真实巡检，可能按策略恢复网络')
    parser.add_argument('--probe', nargs=3, metavar=('HOST', 'PORT', 'INTERFACE'))
    args = parser.parse_args()
    if args.probe:
        host, port, interface = args.probe
        if not policy.INTERFACE.fullmatch(interface) or not 1 <= int(port) <= 65535:
            raise SystemExit(2)
        try:
            raise SystemExit(0 if tcp_probe(host, int(port), interface) else 1)
        except OSError:
            raise SystemExit(1)
    STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    with (STATE_FILE.parent / '.monitor.lock').open('a+') as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise SystemExit('独立网络恢复守护已运行，不能并发执行另一轮恢复')
        monitor = Monitor()
        if args.once:
            print(json.dumps(monitor.tick(), ensure_ascii=False, indent=2))
            return
        stopping = threading.Event()
        for sig in (signal.SIGTERM, signal.SIGINT):
            signal.signal(sig, lambda *_: stopping.set())
        while not stopping.is_set():
            monitor.tick()
            notify('WATCHDOG=1')
            stopping.wait(15)


if __name__ == '__main__':
    main()
