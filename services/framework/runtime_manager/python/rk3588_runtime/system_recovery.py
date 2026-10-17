"""Persistent OS recovery settings. Hardware changes are staged for the next boot."""
from contextlib import contextmanager
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import threading

from .recovery_policy import PolicyConflict
from .watchdog_probe import HARDWARE_DRIVERS

POLICY_FILE = Path('/etc/rk3588-runtime/system-recovery.json')
WATCHDOG_OVERRIDE = Path('/etc/systemd/system.conf.d/zz-rk3588-recovery-switches.conf')
PANIC_OVERRIDE = Path('/etc/sysctl.d/zz-rk3588-recovery-switches.conf')
PANIC_FILE = Path('/proc/sys/kernel/panic')
BOOT_ID_FILE = Path('/proc/sys/kernel/random/boot_id')
MARKER = '# Managed by rk3588_runtime.system_recovery\n'
KEYS = {'hardware_enabled', 'panic_enabled'}
TIMEOUT_KEYS = {'watchdog_timeout_sec', 'reboot_timeout_sec', 'panic_timeout_sec'}
_thread_lock = threading.RLock()


def read():
    try:
        value = json.loads(POLICY_FILE.read_text())
    except FileNotFoundError:
        return None
    except json.JSONDecodeError as exc:
        raise ValueError('系统恢复设置文件格式异常，请管理员检查') from exc
    if (not isinstance(value, dict) or value.get('version') != 1
            or any(type(value.get(key)) is not bool for key in KEYS)
            or not isinstance(value.get('watchdog_device'), (str, type(None)))
            or value.get('watchdog_device') is not None
            and not re.fullmatch(r'/dev/watchdog(?:[0-9]+)?', value['watchdog_device'])
            or any(type(value.get(key)) is not int or not 1 <= value[key] <= 86400
                   for key in ('watchdog_timeout_sec', 'reboot_timeout_sec'))
            or type(value.get('panic_timeout_sec')) is not int or not -2147483648 <= value['panic_timeout_sec'] <= 2147483647
            or value['panic_timeout_sec'] == 0
            or not isinstance(value.get('hardware_change_boot_id'), (str, type(None)))
            or not isinstance(value.get('hardware_timeout_change_boot_id'), (str, type(None)))):
        raise ValueError('系统恢复设置文件格式异常，请管理员检查')
    return value


def _bytes(path):
    try:
        return path.read_bytes()
    except FileNotFoundError:
        return None


@contextmanager
def _locked(write=False):
    with _thread_lock:
        lock = POLICY_FILE.with_suffix('.lock')
        if not write and not lock.exists():
            yield
            return
        if write:
            lock.parent.mkdir(parents=True, exist_ok=True)
        with lock.open('a+' if write else 'r') as handle:
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX if write else fcntl.LOCK_SH)
            try:
                yield
            finally:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def _seconds(value, fallback):
    if not value or value in ('0', 'infinity'):
        return fallback
    if value.isdigit():
        return max(1, min(86400, int(value) // 1000000))
    units = {'h': 3600, 'min': 60, 's': 1, 'ms': .001, 'us': .000001}
    parts = re.findall(r'(\d+(?:\.\d+)?)\s*(min|ms|us|h|s)', value)
    return max(1, min(86400, int(sum(float(n) * units[u] for n, u in parts)))) if parts else fallback


def _same_device(first, second):
    return first == second or {first, second} == {'/dev/watchdog', '/dev/watchdog0'}


def _snapshot(hardware):
    saved = read()
    policy = {key: saved[key] for key in KEYS} if saved else {
        'hardware_enabled': bool(hardware.get('enabled')),
        'panic_enabled': hardware.get('panic_timeout_sec') is not None and hardware['panic_timeout_sec'] != 0,
    }
    policy.update({key: saved[key] for key in TIMEOUT_KEYS} if saved else {
        'watchdog_timeout_sec': _seconds(hardware.get('runtime_timeout'), 60),
        'reboot_timeout_sec': _seconds(hardware.get('reboot_timeout'), 60),
        'panic_timeout_sec': hardware.get('panic_timeout_sec') or 10,
    })
    candidates = [item for item in hardware.get('diagnostics', {}).get('devices', [])
                  if item['driver'] in HARDWARE_DRIVERS and item['node_present']]
    can_enable = not hardware.get('error') and bool(candidates or hardware.get('enabled'))
    pending = bool(saved and (saved['hardware_enabled'] != hardware.get('enabled', False)
                             or saved['hardware_enabled'] and saved['watchdog_device']
                             and not _same_device(saved['watchdog_device'], hardware.get('device'))))
    boot_id = BOOT_ID_FILE.read_text().strip()
    failed = pending and saved['hardware_change_boot_id'] != boot_id
    # The driver can round requested timeouts to hardware-supported values.
    # Track staged changes by boot, rather than requiring exact timeout equality.
    timeout_pending = bool(saved and saved['hardware_enabled']
                           and saved.get('hardware_timeout_change_boot_id') == boot_id)
    current_panic = hardware.get('panic_timeout_sec')
    in_sync = current_panic == (policy['panic_timeout_sec'] if policy['panic_enabled'] else 0)
    revision_data = {'saved': saved, 'policy': policy, 'hardware_actual': hardware.get('enabled'),
                     'device': hardware.get('device'), 'panic': current_panic,
                     'runtime_timeout': hardware.get('runtime_timeout'),
                     'reboot_timeout': hardware.get('reboot_timeout'),
                     'watchdog_override': (_bytes(WATCHDOG_OVERRIDE) or b'').hex(),
                     'panic_override': (_bytes(PANIC_OVERRIDE) or b'').hex()}
    revision = hashlib.sha256(json.dumps(revision_data, sort_keys=True).encode()).hexdigest()
    failure_message = ('设备已重启，但硬件保护未达到保存的设置，请检查设备及系统配置。'
                       if saved and saved['hardware_change_boot_id'] else '当前硬件保护与已保存设置不一致，请检查系统配置。')
    return {'policy': policy, 'revision': revision, 'error': None, 'configured': saved is not None,
            'can_enable_hardware': bool(can_enable), 'can_set_panic': current_panic is not None,
            'hardware_unavailable_reason': None if can_enable else hardware.get('summary') or '未发现可接管的硬件看门狗。',
            'hardware_pending': (pending or timeout_pending) and not failed,
            'hardware_apply_error': failure_message if failed else None,
            'panic_in_sync': in_sync}


def snapshot(hardware):
    try:
        with _locked():
            return _snapshot(hardware)
    except (OSError, ValueError) as exc:
        return {'policy': None, 'revision': None, 'error': str(exc), 'can_enable_hardware': False,
                'can_set_panic': False, 'hardware_pending': False, 'hardware_apply_error': None,
                'panic_in_sync': False, 'hardware_unavailable_reason': '系统恢复设置读取失败。'}


def _atomic(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + '.tmp')
    try:
        with temporary.open('wb') as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        temporary.replace(path)
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    finally:
        temporary.unlink(missing_ok=True)


def save(policy, expected_revision, hardware_reader):
    # Keep accepting switch-only requests from an older frontend during upgrades.
    if (not isinstance(policy, dict) or set(policy) not in (KEYS, KEYS | TIMEOUT_KEYS)
            or any(type(policy.get(key)) is not bool for key in KEYS)):
        raise ValueError('请提交系统恢复开关及完整的超时设置，不支持其他字段')
    if TIMEOUT_KEYS <= set(policy):
        for key, label in (('watchdog_timeout_sec', '卡死复位等待时间'),
                           ('reboot_timeout_sec', '重启阶段看门狗超时')):
            if type(policy[key]) is not int or not 1 <= policy[key] <= 86400:
                raise ValueError(f'{label}必须为 1～86400 秒的整数')
        if (type(policy['panic_timeout_sec']) is not int
                or not -2147483648 <= policy['panic_timeout_sec'] <= 2147483647
                or policy['panic_timeout_sec'] == 0):
            raise ValueError('崩溃重启等待时间必须为非零整数，范围 -2147483648～2147483647 秒；负数表示立即重启')
    with _locked(write=True):
        hardware = hardware_reader()
        current = _snapshot(hardware)
        if current['revision'] != expected_revision:
            raise PolicyConflict('系统恢复设置或实际状态已变化，请重新读取后保存')
        previous = read()
        hardware_changed = policy['hardware_enabled'] != current['policy']['hardware_enabled']
        timeouts_changed = any(policy.get(key, current['policy'][key]) != current['policy'][key]
                               for key in ('watchdog_timeout_sec', 'reboot_timeout_sec'))
        hardware_config_changed = hardware_changed or timeouts_changed and policy['hardware_enabled']
        if policy['hardware_enabled'] and hardware_config_changed and not current['can_enable_hardware']:
            raise ValueError(current['hardware_unavailable_reason'])
        if not current['can_set_panic']:
            raise ValueError('无法读取内核崩溃重启状态，暂不能保存系统恢复设置')
        saved = dict(previous or {
            'version': 1, 'watchdog_device': hardware.get('configured_device') or hardware.get('device'),
            'watchdog_timeout_sec': _seconds(hardware.get('runtime_timeout'), 60),
            'reboot_timeout_sec': _seconds(hardware.get('reboot_timeout'), 60),
            'panic_timeout_sec': hardware.get('panic_timeout_sec') or 10,
            'hardware_change_boot_id': None,
        })
        if hardware_changed and policy['hardware_enabled']:
            candidates = [item for item in hardware['diagnostics']['devices']
                          if item['driver'] in HARDWARE_DRIVERS and item['node_present']]
            candidates.sort(key=lambda item: (item['device'] != hardware.get('device'),
                                              item['driver'] != 'dw_wdt', item['device']))
            if not candidates:
                raise ValueError('没有可接管的硬件看门狗，请先检查设备状态')
            saved['watchdog_device'] = candidates[0]['device']
        if hardware_config_changed or hardware.get('reboot_required') and not previous:
            saved['hardware_change_boot_id'] = BOOT_ID_FILE.read_text().strip()
        if timeouts_changed and policy['hardware_enabled']:
            saved['hardware_timeout_change_boot_id'] = BOOT_ID_FILE.read_text().strip()
        if 'panic_timeout_sec' not in policy and hardware.get('panic_timeout_sec'):
            saved['panic_timeout_sec'] = hardware['panic_timeout_sec']
        saved.update(policy)
        updates = {}
        if hardware_config_changed:
            text = MARKER + '[Manager]\n'
            if policy['hardware_enabled']:
                text += f"WatchdogDevice={saved['watchdog_device']}\n"
            text += f"RuntimeWatchdogSec={saved['watchdog_timeout_sec'] if policy['hardware_enabled'] else 0}s\n"
            text += f"RebootWatchdogSec={saved['reboot_timeout_sec'] if policy['hardware_enabled'] else 0}s\n"
            updates[WATCHDOG_OVERRIDE] = text.encode()
        panic_timeout = saved['panic_timeout_sec'] if policy['panic_enabled'] else 0
        updates[PANIC_OVERRIDE] = (MARKER + f'kernel.panic={panic_timeout}\n').encode()
        updates[POLICY_FILE] = (json.dumps(saved, ensure_ascii=False, indent=2) + '\n').encode()
        old = {path: _bytes(path) for path in updates}
        for path, content in old.items():
            if path.is_symlink():
                raise ValueError(f'发现自定义配置链接，未覆盖：{path}')
            if path != POLICY_FILE and content is not None and not content.startswith(MARKER.encode()):
                raise ValueError(f'发现自定义配置，未覆盖：{path}')
        old_panic = hardware['panic_timeout_sec']
        touched = []
        panic_touched = False
        try:
            for path, content in updates.items():
                touched.append(path)
                _atomic(path, content)
            if old_panic != panic_timeout:
                panic_touched = True
                PANIC_FILE.write_text(str(panic_timeout) + '\n')
                if int(PANIC_FILE.read_text().strip()) != panic_timeout:
                    raise RuntimeError('内核未接受崩溃重启设置')
        except Exception as exc:
            rollback_errors = []
            for path in reversed(touched):
                try:
                    if old[path] is None:
                        path.unlink(missing_ok=True)
                    else:
                        _atomic(path, old[path])
                except OSError as rollback_error:
                    rollback_errors.append(str(rollback_error))
            if panic_touched:
                try:
                    PANIC_FILE.write_text(str(old_panic) + '\n')
                except OSError as rollback_error:
                    rollback_errors.append(str(rollback_error))
            if rollback_errors:
                raise RuntimeError('保存失败，部分设置回滚失败，请管理员检查：' + '；'.join(rollback_errors)) from exc
            raise RuntimeError(f'保存失败，已恢复原设置：{exc}') from exc
    # Never daemon-reexec, stop feeding, or reboot from this operation.
