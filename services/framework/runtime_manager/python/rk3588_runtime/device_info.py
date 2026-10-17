"""Read-only device overview. Short-lived RAM cache; no probes or disk history."""
from concurrent.futures import ThreadPoolExecutor
import copy
from datetime import datetime
import json
import os
from pathlib import Path
import platform
import re
import shlex
import socket
import subprocess
import threading
import time

from . import fault_diagnostics as diagnostics, manager, recovery_history, performance

PROC = Path('/proc')
SYS = Path('/sys')
ETC = Path('/etc')
SERVICES = {
    'rk3588-console.service': 'Web 控制台',
    'rk3588-runtime-manager.service': '程序运行协调',
    'rk3588-network-recovery.service': '网络故障恢复',
    'rk3588-recovery-history.service': '记录与故障取证',
    'NetworkManager.service': '网络管理',
    'systemd-journald.service': '系统日志',
    'ota_agent.service': '升级服务',
    'unified_upload.service': '上传服务',
}
_lock = threading.RLock()
_cached = None
_cached_at = 0
_cpu_previous = None
_cpu_usage = None


def read(path, limit=4096):
    return diagnostics.read(path, limit)


def optional(path, limit=4096):
    try:
        return read(path, limit)
    except OSError:
        return ''


def cpu_usage():
    global _cpu_previous, _cpu_usage
    line = read(PROC / 'stat').splitlines()[0].split()
    if not line or line[0] != 'cpu' or len(line) < 5:
        raise ValueError('CPU 计数不可用')
    counts = [int(number) for number in line[1:9]]  # Guest time is already included in user/nice.
    total, idle = sum(counts), counts[3] + (counts[4] if len(counts) > 4 else 0)
    if _cpu_previous and total > _cpu_previous[0]:
        delta = total - _cpu_previous[0]
        _cpu_usage = round(max(0, min(100, 100 * (1 - (idle - _cpu_previous[1]) / delta))), 1)
    elif _cpu_previous and total < _cpu_previous[0]:
        _cpu_usage = None
    _cpu_previous = total, idle
    return _cpu_usage


def cpu_info():
    values = []
    for directory in sorted((SYS / 'devices/system/cpu/cpufreq').glob('policy*'))[:16]:
        for name in ('cpuinfo_max_freq', 'scaling_max_freq'):
            text = optional(directory / name, 32)
            if text.isdigit() and 0 < int(text) < 10000000:
                values.append(int(text) / 1000)
                break
    sampling = True
    try:
        usage = cpu_usage()
    except (OSError, ValueError, IndexError):
        usage = None
        sampling = False
    load = optional(PROC / 'loadavg', 120).split()[:3]
    return {'architecture': platform.machine(), 'cores': os.cpu_count(), 'usage_percent': usage,
            'usage_sampling': sampling, 'max_mhz': max(values) if values else None,
            'load_average': [float(value) for value in load] if len(load) == 3 else []}


def memory_info():
    values = {key: int(value) * 1024 for key, value in re.findall(r'^(\w+):\s+(\d+) kB', read(PROC / 'meminfo'), re.M)}
    total = values.get('MemTotal', 0)
    if not total:
        raise ValueError('内存容量不可用')
    available = values.get('MemAvailable')
    estimated = available is None
    if estimated:
        available = sum(values.get(key, 0) for key in ('MemFree', 'Buffers', 'Cached', 'SReclaimable')) - values.get('Shmem', 0)
    available = min(total, max(0, available))
    swap_total = values.get('SwapTotal', 0)
    return {'total_bytes': total, 'available_bytes': available, 'used_bytes': total - available,
            'usage_percent': round((total - available) / total * 100, 1), 'available_estimated': estimated,
            'swap_total_bytes': swap_total, 'swap_used_bytes': max(0, swap_total - values.get('SwapFree', 0))}


def system_info(board):
    release = {}
    for line in optional(ETC / 'os-release', 8192).splitlines():
        key, separator, value = line.partition('=')
        if separator and key in ('PRETTY_NAME', 'NAME', 'VERSION_ID'):
            try:
                release[key] = ' '.join(shlex.split(value))[:160]
            except ValueError:
                continue
    try:
        uptime = float(read(PROC / 'uptime', 120).split()[0])
    except (OSError, ValueError, IndexError):
        uptime = None
    now = time.time()
    try:
        timezone = str((ETC / 'localtime').resolve()).partition('/zoneinfo/')[2]
    except OSError:
        timezone = ''
    timezone = timezone or datetime.now().astimezone().tzname()
    serial = optional(SYS / 'firmware/devicetree/base/serial-number', 160)
    if not serial:
        match = re.search(r'^Serial\s*:\s*(\S+)', optional(PROC / 'cpuinfo', 128 * 1024), re.M)
        serial = match[1] if match else ''
    compatible = board.get('compatible', '')
    soc = next((item.split(',')[-1] for item in re.findall(r'rockchip,rk[0-9a-z]+', compatible)
                if not re.search(r'-', item)), '')
    return {'hostname': optional(PROC / 'sys/kernel/hostname', 160) or socket.gethostname(),
            'os_name': release.get('PRETTY_NAME') or release.get('NAME') or '未提供',
            'kernel': board.get('kernel') or platform.release(), 'soc': soc.upper() or None,
            'serial': serial or None, 'uptime_sec': max(0, int(uptime)) if uptime is not None else None,
            'boot_time': now - uptime if uptime is not None else None,
            'current_time': datetime.now().astimezone().isoformat(timespec='seconds'), 'timezone': timezone,
            'application_directory': os.environ.get('APPS_ROOT', '/opt/ai_apps')}


def unescape_mount(text):
    return re.sub(r'\\(040|011|012|134)', lambda match: chr(int(match[1], 8)), text)


def storage_info():
    local = {'ext2', 'ext3', 'ext4', 'f2fs', 'btrfs', 'xfs', 'vfat', 'exfat', 'ntfs', 'ntfs3', 'fuseblk', 'overlay', 'zfs'}
    app_path = Path(os.environ.get('APPS_ROOT', '/opt/ai_apps')).resolve()
    values, seen = [], set()
    for line in read(PROC / 'mounts', 64 * 1024).splitlines()[:256]:
        parts = line.split()
        if len(parts) < 4 or parts[2] not in local:
            continue
        source, mount = unescape_mount(parts[0]), unescape_mount(parts[1])
        if parts[2] == 'overlay' and mount != '/':
            continue
        try:
            device = os.stat(mount).st_dev
            if device in seen:
                continue
            stat = os.statvfs(mount)
            total = stat.f_blocks * stat.f_frsize
            if not total:
                continue
            used = max(0, total - stat.f_bfree * stat.f_frsize)
            available = max(0, stat.f_bavail * stat.f_frsize)
            values.append({'mount': mount, 'source': source[:160], 'filesystem': parts[2],
                           'total_bytes': total, 'used_bytes': used, 'available_bytes': available,
                           'usage_percent': round(used / total * 100, 1),
                           'readonly': bool(stat.f_flag & os.ST_RDONLY), 'application_data': False})
            seen.add(device)
        except OSError:
            continue
        if len(values) == 8:
            break
    # The application path may be a symlink into a separate userdata partition.
    matches = [item for item in values if Path(item['mount']) == app_path or Path(item['mount']) in app_path.parents]
    if matches:
        max(matches, key=lambda item: len(item['mount']))['application_data'] = True
    return values


def temperature_info():
    groups = {}
    for directory in sorted((SYS / 'class/thermal').glob('thermal_zone*'))[:16]:
        try:
            kind = read(directory / 'type', 80)
            value = int(read(directory / 'temp', 32)) / 1000
            if not -40 <= value <= 200:
                continue
            label = next((title for pattern, title in [('gpu|gfx', 'GPU'), ('npu', 'NPU'),
                          ('cpu|bigcore|littlecore', 'CPU'), ('soc|package', '芯片'), ('pmic', '电源')]
                          if re.search(pattern, kind, re.I)), '其他传感器')
            item = {'label': label, 'celsius': value, 'critical_celsius': None, 'critical_reached': False}
            for trip in sorted(directory.glob('trip_point_*_type'))[:16]:
                try:
                    if read(trip, 32) == 'critical':
                        critical = int(read(trip.with_name(trip.name.replace('_type', '_temp')), 32)) / 1000
                        if 0 < critical <= 200:
                            item['critical_celsius'] = critical
                            item['critical_reached'] |= value >= critical
                except (OSError, ValueError):
                    continue
            previous = groups.get(label)
            if previous:
                item['critical_reached'] |= previous['critical_reached']
                previous['critical_reached'] = item['critical_reached']
            if previous is None or previous['celsius'] < value:
                groups[label] = item
        except (OSError, ValueError):
            continue
    return list(groups.values())


def accelerator_info():
    result = []
    for directory in sorted((SYS / 'class/devfreq').glob('*'))[:32]:
        name = optional(directory / 'name', 100) or directory.name
        kind = 'GPU' if re.search(r'gpu|mali|panfrost|panthor', name, re.I) else 'NPU' if re.search('npu', name, re.I) else None
        if not kind:
            continue
        text = optional(directory / 'cur_freq', 32)
        driver = (directory / 'device/driver').resolve().name if (directory / 'device/driver').exists() else None
        result.append({'kind': kind, 'driver': driver, 'registered': True,
                       'current_mhz': int(text) / 1000000 if text.isdigit() else None})
        if len(result) == 4:
            break
    return result


def network_info(addresses, routes4, routes6):
    if not isinstance(addresses, list):
        raise ValueError('网卡信息格式异常')
    routes = [item for group in (routes4, routes6) if isinstance(group, list) for item in group if isinstance(item, dict)]
    interfaces = []
    for item in addresses:
        if not isinstance(item, dict):
            continue
        name = item.get('ifname', '')
        if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_.:@-]{1,64}', name) or name == 'lo' or name.startswith(('veth', 'docker', 'virbr')):
            continue
        base = SYS / 'class/net' / name
        speed = optional(base / 'speed', 24)
        ips = [f"{entry['local']}/{entry.get('prefixlen', '')}" for entry in item.get('addr_info', [])
               if isinstance(entry, dict) and entry.get('local') and entry.get('scope') == 'global'
               and not set(entry.get('flags', [])) & {'tentative', 'dadfailed'}][:8]
        physical = (base / 'device').exists() or (base / 'wireless').exists() or (base / 'phy80211').exists()
        if name.startswith('dummy') or (not physical and 'UP' not in item.get('flags', []) and 'LOWER_UP' not in item.get('flags', [])):
            continue
        interfaces.append({'name': name, 'kind': 'Wi-Fi' if (base / 'wireless').exists() or (base / 'phy80211').exists() else '网卡',
                           'link_up': 'LOWER_UP' in item.get('flags', []), 'mac': str(item.get('address', ''))[:64],
                           'addresses': ips, 'speed_mbps': int(speed) if speed.isdigit() and int(speed) > 0 else None,
                           'gateways': list(dict.fromkeys(str(route['gateway']) for route in routes
                                           if route.get('dev') == name and route.get('gateway')))[:4]})
        if len(interfaces) == 8:
            break
    dns = re.findall(r'^\s*nameserver\s+(\S+)', optional(ETC / 'resolv.conf', 8192), re.M)[:4]
    return {'interfaces': interfaces, 'dns': dns}


def service_info(text):
    states = {}
    for block in text.strip().split('\n\n'):
        values = dict(line.split('=', 1) for line in block.splitlines() if '=' in line)
        states[values.get('Id')] = values
    return [{'unit': unit, 'label': label, 'load_state': states.get(unit, {}).get('LoadState', 'unknown'),
             'active_state': states.get(unit, {}).get('ActiveState', 'unknown'),
             'sub_state': states.get(unit, {}).get('SubState', ''),
             'autostart': True if states.get(unit, {}).get('UnitFileState') in ('enabled', 'enabled-runtime') else
             False if states.get(unit, {}).get('UnitFileState') in ('disabled', 'masked', 'masked-runtime') else None}
            for unit, label in SERVICES.items()]


def application_info():
    path = manager.STATUS_FILE
    if not path.exists() or path.stat().st_size > 64 * 1024:
        return {'state': 'unavailable'}
    state = json.loads(path.read_text())
    if not isinstance(state, dict) or state.get('boot_id') != recovery_history.boot_id() or not 0 <= time.time() - state.get('updated_at', 0) <= 30:
        return {'state': 'unavailable'}
    if state.get('phase') == 'disabled':
        return {'state': 'disabled'}
    current = state.get('current')
    if isinstance(current, dict) and current.get('app'):
        return {'state': current.get('status', 'unknown'), 'name': str(current['app'])[:160],
                'config': str(current.get('config') or '')[:160]}
    return {'state': 'retrying' if state.get('phase') == 'retrying' else 'idle'}


def overview():
    global _cached, _cached_at
    with _lock:
        if _cached is not None and 0 <= time.monotonic() - _cached_at < 0.5:
            return copy.deepcopy(_cached)
        errors = []
        def section(label, callback, fallback):
            try:
                return callback()
            except (OSError, ValueError, TypeError, subprocess.SubprocessError, RuntimeError) as exc:
                errors.append(label + '：' + str(exc)[:160])
                return fallback

        jobs = {
            '网卡地址': ['ip', '-j', 'address', 'show'],
            'IPv4 默认路由': ['ip', '-j', 'route', 'show', 'default'],
            'IPv6 默认路由': ['ip', '-j', '-6', 'route', 'show', 'default'],
            '关键服务': ['systemctl', 'show', *SERVICES, '--property=Id,LoadState,ActiveState,SubState,UnitFileState'],
            '时间同步': ['timedatectl', 'show', '--property=NTP,NTPSynchronized'],
        }
        def command(job):
            label, args = job
            try:
                return label, diagnostics.bounded_command(args), None
            except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
                return label, '', str(exc)[:160]
        with ThreadPoolExecutor(max_workers=5) as executor:
            output = {}
            for label, text, failure in executor.map(command, jobs.items()):
                output[label] = text
                if failure:
                    errors.append(label + '：' + failure)
        board = section('板卡信息', diagnostics.board_capabilities, {'limitations': []})
        evidence = diagnostics.status()
        evidence['board'] = board
        ntp = dict(line.split('=', 1) for line in output['时间同步'].splitlines() if '=' in line)
        addresses = section('网卡信息解析', lambda: json.loads(output['网卡地址']), [])
        routes4 = section('IPv4 路由解析', lambda: json.loads(output['IPv4 默认路由'] or '[]'), [])
        routes6 = section('IPv6 路由解析', lambda: json.loads(output['IPv6 默认路由'] or '[]'), [])
        network = section('网络信息', lambda: network_info(addresses, routes4, routes6),
                          {'interfaces': [], 'dns': []})
        system = section('系统概况', lambda: system_info(board), {})
        system.update(ntp_enabled={'yes': True, 'no': False}.get(ntp.get('NTP')),
                      ntp_synchronized={'yes': True, 'no': False}.get(ntp.get('NTPSynchronized')))
        value = {'updated_at': time.time(), 'system': system,
                 'cpu': {}, 'memory': {}, 'storage': [], 'temperatures': [], 'accelerators': [], 'network': network,
                 'services': service_info(output['关键服务']),
                 'application': {'state': 'unavailable'},
                 'diagnostics': evidence, 'observer': manager.read_json(Path('/run/rk3588-recovery-history/status.json'), {}),
                 'errors': errors[:12]}
        _cached, _cached_at = value, time.monotonic()
        return copy.deepcopy(value)


def snapshot():
    # Slow system commands are cached separately from one-second live counters.
    value = overview()
    with _lock:
        for name, callback, fallback in [('cpu', cpu_info, {}), ('memory', memory_info, {}),
                ('storage', storage_info, []), ('temperatures', temperature_info, []),
                ('accelerators', accelerator_info, []), ('application', application_info, {'state': 'unavailable'}),
                ('performance', performance.snapshot, None)]:
            try:
                value[name] = callback()
            except (OSError, ValueError, TypeError, RuntimeError) as exc:
                value[name] = fallback
                value['errors'] = (value['errors'] + [name + '：' + str(exc)[:160]])[-12:]
        system = system_info(value['diagnostics'].get('board', {}))
        value['system'].update(system)
        value['updated_at'] = time.time()
    return value
