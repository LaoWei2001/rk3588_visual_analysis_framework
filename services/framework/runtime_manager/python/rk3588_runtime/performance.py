"""Bounded performance telemetry and validated, persistent kernel limits.

Only discovered CPUFreq/devfreq and thermal trip files may be written. Debugfs
load/version are read only; voltage, power, reset and thermal mode are untouched.
"""
from contextlib import contextmanager
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import tempfile
import threading
import time

from . import recovery_history
from .recovery_policy import PolicyConflict

SYS = Path('/sys')
PROC = Path('/proc')
PROFILE = Path('/etc/rk3588-runtime/performance.json')
MAX_BYTES = 64 * 1024
_lock = threading.RLock()


def read(path):
    try:
        with path.open('rb') as handle:
            return handle.read(8192).decode('utf-8', errors='replace').replace('\0', ' ').strip()
    except OSError:
        return ''


def number(path, scale=1):
    try:
        value = float(read(path)) / scale
        return value if math.isfinite(value) else None
    except ValueError:
        return None


def writable(path):
    try:
        return bool(path.stat().st_mode & 0o222) and os.access(path, os.W_OK)
    except OSError:
        return False


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def load_profile():
    try:
        with PROFILE.open('rb') as handle:
            data = handle.read(MAX_BYTES + 1)
        if len(data) > MAX_BYTES:
            raise ValueError('性能设置文件超过 64 KiB 限制')
        value = json.loads(data)
        if (not isinstance(value, dict) or value.get('version') != 1
                or not isinstance(value.get('limits'), dict) or not isinstance(value.get('defaults'), dict)
                or not isinstance(value.get('fingerprint'), str)):
            raise ValueError('性能设置文件格式异常')
        return value
    except FileNotFoundError:
        return None


def discover():
    devices, trips, controls, temperatures = [], [], {}, {}
    for zone in sorted((SYS / 'class/thermal').glob('thermal_zone*'))[:16]:
        kind, temp = read(zone / 'type'), number(zone / 'temp', 1000)
        if temp is not None and -40 <= temp <= 200:
            group = next((name for pattern, name in [('gpu|gfx', 'GPU'), ('npu', 'NPU'),
                         ('cpu|bigcore|littlecore', 'CPU'), ('soc|package', 'SoC')]
                         if re.search(pattern, kind, re.I)), '其他')
            temperatures[group] = max(temp, temperatures.get(group, -40))
        defaults = {}
        dt = SYS / 'firmware/devicetree/base/thermal-zones' / kind / 'trips'
        for entry in sorted(dt.glob('*'))[:16]:
            try:
                raw = (entry / 'temperature').read_bytes()
                if len(raw) == 4:
                    defaults.setdefault(read(entry / 'type'), []).append(int.from_bytes(raw, 'big') / 1000)
            except OSError:
                continue
        for values in defaults.values():
            values.sort()
        indexes = {}
        for file in sorted(zone.glob('trip_point_*_type'), key=lambda p: int(p.name.split('_')[2]))[:16]:
            trip_type = read(file)
            if trip_type not in ('passive', 'hot', 'critical'):
                continue
            path = file.with_name(file.name.replace('_type', '_temp'))
            value = number(path, 1000)
            if value is None or not 0 < value <= 150:
                continue
            i = indexes.get(trip_type, 0)
            indexes[trip_type] = i + 1
            baseline = (defaults.get(trip_type, []) + [None] * 16)[i]
            key = f'thermal:{kind}:{file.name.split("_")[2]}'
            if key in controls:
                key += ':' + zone.name
            item = {'id': key, 'zone': kind, 'zone_id': zone.name, 'kind': trip_type, 'celsius': value,
                    'hysteresis_celsius': number(path.with_name(path.name.replace('_temp', '_hyst')), 1000) or 0,
                    'default_celsius': baseline, 'ceiling_celsius': baseline if trip_type == 'critical' else None,
                    'editable': writable(path) and (trip_type != 'critical' or baseline is not None),
                    'sensor_celsius': temp}
            trips.append(item)
            controls[key] = (item, path, 1000)
    roots = [(path, 'CPU', 1000) for path in sorted((SYS / 'devices/system/cpu/cpufreq').glob('policy*'))[:16]]
    for path in sorted((SYS / 'class/devfreq').glob('*'))[:32]:
        name = read(path / 'name') or path.name
        kind = 'GPU' if re.search('gpu|mali|panfrost|panthor', name, re.I) else 'NPU' if re.search('npu', name, re.I) else None
        if kind:
            roots.append((path, kind, 1000000))
        if len(roots) >= 20:
            break
    for path, kind, scale in roots:
        cpu = kind == 'CPU'
        prefix = 'scaling_' if cpu else ''
        cores = (read(path / 'related_cpus') or read(path / 'affected_cpus')) if cpu else ''
        key = f'CPU:{cores or path.name}' if cpu else f'{kind}:{path.name}'
        available = sorted({int(n) / scale for n in read(path / ('scaling_available_frequencies' if cpu else 'available_frequencies')).split() if n.isdigit() and int(n) > 0})[:64]
        minimum, maximum = number(path / (prefix + 'min_freq'), scale), number(path / (prefix + 'max_freq'), scale)
        load = None
        source = None
        core_loads = []
        if kind == 'GPU':
            match = re.fullmatch(r'(\d+)(?:@\d+Hz)?', read(path / 'load'))
            if match and 0 <= int(match[1]) <= 100:
                load, source = int(match[1]), 'devfreq/load'
        if kind == 'NPU':
            for loadfile in (SYS / 'kernel/debug/rknpu/load', PROC / 'rknpu/load'):
                pairs = re.findall(r'Core\s*(\d+)\s*:\s*(\d+)\s*%', read(loadfile), re.I)[:8]
                if pairs and all(0 <= int(value) <= 100 for _, value in pairs):
                    core_loads = [{'core': int(core), 'usage_percent': int(value)} for core, value in pairs]
                    load = round(sum(item['usage_percent'] for item in core_loads) / len(core_loads), 1)
                    source = str(loadfile)
                    break
        item = {'id': key, 'kind': kind, 'label': f'CPU 核心 {cores}' if cpu and cores else path.name if cpu else kind,
                'cores': cores, 'current_mhz': number(path / (prefix + 'cur_freq'), scale),
                'min_mhz': minimum, 'max_mhz': maximum, 'available_mhz': available,
                'governor': read(path / (prefix + 'governor')) or None,
                'usage_percent': load, 'usage_source': source, 'core_loads': core_loads,
                'temperature_celsius': temperatures.get(kind),
                'driver': read(SYS / 'kernel/debug/rknpu/version') if kind == 'NPU' else None,
                'editable': bool(available) and minimum is not None and maximum is not None
                            and writable(path / (prefix + 'min_freq')) and writable(path / (prefix + 'max_freq'))}
        devices.append(item)
        controls[key] = (item, path / (prefix + 'max_freq'), scale)
    board = [read(SYS / ('firmware/devicetree/base/' + name)) for name in ('model', 'compatible', 'serial-number')]
    if not board[2]:
        serial = re.search(r'^Serial\s*:\s*(\S+)', read(PROC / 'cpuinfo'), re.M)
        board[2] = serial[1] if serial else ''
    fingerprint = digest([board, read(PROC / 'sys/kernel/osrelease') or platform.release(),
                          [(d['id'], d['available_mhz']) for d in devices],
                          [(t['id'], t['zone_id'], t['kind'], t['default_celsius']) for t in trips]])
    return devices, trips, controls, fingerprint


def _snapshot():
    devices, trips, _, fingerprint = discover()
    error, profile = None, None
    try:
        profile = load_profile()
    except (OSError, ValueError, TypeError) as exc:
        error = str(exc)
    matches = profile is not None and profile['fingerprint'] == fingerprint
    actual = {item['id']: item['max_mhz'] for item in devices}
    actual.update({item['id']: item['celsius'] for item in trips})
    applied = matches and all(actual.get(key) == value for key, value in profile['limits'].items())
    return {'sampled_at': time.time(), 'devices': devices, 'thermal_trips': trips,
            'revision': digest([fingerprint, actual, profile, error]),
            'saved': profile is not None, 'applied': bool(applied), 'profile_matches': matches,
            'error': error, 'notice': ('板卡或内核接口已变化，旧设置未自动应用。' if profile and not matches else
                     '当前内核参数与已保存设置不同。' if matches and not applied else None)}


def snapshot():
    with _lock:
        return _snapshot()


@contextmanager
def mutation_lock():
    with _lock:
        PROFILE.parent.mkdir(parents=True, exist_ok=True)
        with PROFILE.with_suffix('.lock').open('a+') as handle:
            fcntl.flock(handle, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(handle, fcntl.LOCK_UN)


def write_value(path, value):
    with path.open('w') as handle:
        handle.write(str(value))
    # CPUFreq applies QoS requests asynchronously on a work queue.
    deadline = time.monotonic() + 0.5
    while number(path) != value:
        if time.monotonic() >= deadline:
            raise RuntimeError(f'内核未接受设置：{path.name}，请检查驱动限制')
        time.sleep(0.02)


def persist(profile):
    data = json.dumps(profile, ensure_ascii=False, sort_keys=True).encode()
    if len(data) > MAX_BYTES:
        raise ValueError('性能设置超过大小限制')
    fd, name = tempfile.mkstemp(prefix='.performance-', dir=PROFILE.parent)
    try:
        with os.fdopen(fd, 'wb') as handle:
            handle.write(data)
            handle.flush()
            os.fsync(handle.fileno())
        os.chmod(name, 0o600)
        os.replace(name, PROFILE)
        try:
            directory = os.open(PROFILE.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        except OSError:
            # Atomic replacement has committed; do not falsely report an unapplied setting.
            pass
    finally:
        Path(name).unlink(missing_ok=True)


def validate(limits, devices, trips, controls):
    if not isinstance(limits, dict) or len(limits) > 276 or set(limits) - set(controls):
        raise ValueError('存在未知的性能设置项目')
    for key, value in limits.items():
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
            raise ValueError('性能设置必须是有效数字')
        item, _, scale = controls[key]
        if not item['editable']:
            raise ValueError(f'{key} 未提供可写的内核接口')
        if scale != 1000 or item.get('kind') == 'CPU':
            if value not in item['available_mhz']:
                raise ValueError(f'{key} 的上限必须选用内核支持的频率档位')
        else:
            if value * 10 != round(value * 10) or not 20 <= value <= 150:
                raise ValueError('温度阈值须为 20～150 ℃，最多一位小数')
            if item['kind'] == 'critical' and value > item['ceiling_celsius']:
                raise ValueError('临界保护温度不得超过设备树原始上限')
            # Unchanged existing values need not satisfy a newly raised live temperature.
            margin = 10 if item['kind'] in ('critical', 'hot') else 2
            if value != item['celsius'] and item['sensor_celsius'] is not None and value <= item['sensor_celsius'] + margin:
                raise ValueError(f'{item["zone"]} 阈值须高于当前温度至少 {margin} ℃')
    for zone in {item['zone_id'] for item in trips}:
        group = [item for item in trips if item['zone_id'] == zone]
        ordered = sorted(group, key=lambda item: ({'passive': 0, 'hot': 1, 'critical': 2}[item['kind']],
                         item['default_celsius'] if item['default_celsius'] is not None else item['celsius'], item['id']))
        for lower, upper in zip(ordered, ordered[1:]):
            gap = max(2, lower['hysteresis_celsius'])
            if limits.get(lower['id'], lower['celsius']) + gap > limits.get(upper['id'], upper['celsius']):
                raise ValueError('同一温控区的阈值须按升温顺序排列，并保留回差间隔')


def execute(limits, controls, min_targets=None):
    """Apply verified sysfs changes, recording before write to cover readback failure."""
    changes = []
    def change(path, target):
        previous = number(path)
        if previous is None:
            raise RuntimeError(f'无法读取 {path.name}')
        target = int(round(target))
        if target != previous:
            changes.append((path, int(previous)))
            write_value(path, target)
    try:
        thermal = []
        for key, value in limits.items():
            item, path, scale = controls[key]
            if 'zone' in item:
                thermal.append((key, value))
                continue
            if item['kind'] == 'CPU':
                # scaling_min_freq includes transient PM QoS boosts. Do not turn
                # an effective boost into a persistent userspace minimum request.
                change(path, value * scale)
                continue
            min_path = path.with_name(path.name.replace('max_freq', 'min_freq'))
            old_min = number(min_path)
            target_min = (min_targets or {}).get(key, min(old_min / scale, value))
            if target_min > value:
                raise ValueError('最小频率不能超过最大频率')
            if target_min * scale < old_min:
                change(min_path, target_min * scale)
            change(path, value * scale)
            change(min_path, target_min * scale)
        # Lower thresholds from the bottom up, raise thresholds from the top down.
        thermal.sort(key=lambda pair: (pair[1] >= controls[pair[0]][0]['celsius'],
                                      pair[1] if pair[1] < controls[pair[0]][0]['celsius'] else -pair[1]))
        for key, value in thermal:
            change(controls[key][1], value * 1000)
        for key, value in limits.items():
            _, path, scale = controls[key]
            if number(path, scale) != value:
                raise RuntimeError(f'应用后参数发生变化：{key}')
        return changes
    except Exception as exc:
        rollback(changes, exc)
        raise


def rollback(changes, cause):
    failures = []
    for path, previous in reversed(changes):
        try:
            if number(path) != previous:
                write_value(path, previous)
        except Exception as exc:
            failures.append(str(exc))
    if failures:
        raise RuntimeError(f'设置失败：{cause}；部分参数回退失败：{"；".join(failures)}') from cause


def _save(payload, reset=False):
    if (not isinstance(payload, dict) or set(payload) != ({'revision'} if reset else {'revision', 'limits'})
            or not isinstance(payload.get('revision'), str)):
        raise ValueError('请提交设置和读取时的版本号')
    with mutation_lock():
        current = _snapshot()
        if current['error']:
            raise ValueError(current['error'])
        if payload['revision'] != current['revision']:
            raise PolicyConflict('性能参数已变化，请放弃修改后按当前值重新设置')
        devices, trips, controls, fingerprint = discover()
        old = load_profile()
        matches = old is not None and old['fingerprint'] == fingerprint
        defaults = old['defaults'] if matches else {
            'limits': {**{d['id']: d['max_mhz'] for d in devices if d['editable']},
                       **{t['id']: t['default_celsius'] if t['default_celsius'] is not None else t['celsius'] for t in trips if t['editable']}},
            'minimums': {d['id']: d['min_mhz'] for d in devices if d['editable'] and d['kind'] != 'CPU'}}
        limits = defaults['limits'] if reset else payload['limits']
        if not limits:
            raise ValueError('没有可保存的性能设置')
        validate(limits, devices, trips, controls)
        retained = old['limits'].copy() if matches else {}
        retained.update(limits)
        minimums = defaults['minimums'] if reset else None
        changes = execute(limits, controls, minimums)
        profile = {'version': 1, 'fingerprint': fingerprint, 'limits': retained, 'defaults': defaults}
        try:
            persist(profile)
        except Exception as exc:
            rollback(changes, exc)
            raise
        recovery_history.safe_record('system', 'performance_settings', '用户调整设备性能参数',
            '恢复初始频率与温控设置' if reset else '设置 CPU / GPU / NPU 频率上限与内核温控阈值',
            result='performed', detail=json.dumps(limits, ensure_ascii=False)[:1200], source='性能设置', fault_recovery=False)
        return _snapshot()


def save(payload, reset=False):
    try:
        return _save(payload, reset)
    except Exception as exc:
        recovery_history.safe_record('system', 'performance_settings', '性能设置未成功应用',
            '校验参数或回退已写入的内核参数', result='failed', detail=str(exc),
            source='性能设置', fault_recovery=False)
        raise


def apply_saved():
    with mutation_lock():
        profile = load_profile()
        if profile is None:
            return {'applied': False, 'reason': '未保存性能设置'}
        devices, trips, controls, fingerprint = discover()
        if profile['fingerprint'] != fingerprint:
            raise ValueError('板卡或内核接口发生变化，未应用旧性能设置')
        validate(profile['limits'], devices, trips, controls)
        execute(profile['limits'], controls)
        recovery_history.safe_record('system', 'performance_settings', '开机恢复已保存的性能参数',
            '重新应用频率上限和温控阈值并读取校验', result='performed', source='性能设置服务', fault_recovery=False)
        return {'applied': True}


if __name__ == '__main__':
    try:
        print(json.dumps(apply_saved(), ensure_ascii=False))
    except Exception as exc:
        recovery_history.safe_record('system', 'performance_settings', '开机应用性能设置失败',
            '保留内核现有参数', result='failed', detail=str(exc), source='性能设置服务', fault_recovery=False)
        raise
