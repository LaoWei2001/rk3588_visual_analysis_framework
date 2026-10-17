"""Persistent, validated device recovery policy. No HTTP or third-party dependencies."""
import fcntl
import hashlib
import json
import os
from pathlib import Path

POLICY_FILE = Path(os.environ.get('RUNTIME_POLICY_FILE', '/etc/rk3588-runtime/recovery.json'))
DEFAULTS = {
    'manager_enabled': True,
    # Device recovery protects the OS and network. Application recovery is a
    # legacy opt-in, independent of normal boot autostart coordination.
    'app_restart_enabled': False,
    'app_stall_enabled': False,
    'restart_delay_sec': 5,
    'task_timeout_sec': 120,
    'startup_timeout_sec': 300,
    'kernel_reboot_enabled': False,
    'kernel_hang_sec': 180,
    'reboot_cooldown_sec': 1800,
    'max_reboots_per_day': 3,
}
RANGES = {
    'restart_delay_sec': (5, 300),
    'task_timeout_sec': (30, 1800),
    'startup_timeout_sec': (60, 1800),
    'kernel_hang_sec': (120, 1800),
    'reboot_cooldown_sec': (600, 86400),
    'max_reboots_per_day': (1, 10),
}
APP_KEYS = ('app_restart_enabled', 'app_stall_enabled', 'restart_delay_sec',
            'task_timeout_sec', 'startup_timeout_sec')


class PolicyConflict(RuntimeError):
    pass


def validate(raw):
    if not isinstance(raw, dict) or set(raw) - set(DEFAULTS):
        raise ValueError('恢复策略包含未知字段或格式错误')
    value = {**DEFAULTS, **raw}
    for key, default in DEFAULTS.items():
        if type(value[key]) is not type(default):
            raise ValueError(f'{key} 的类型不正确')
        if key in RANGES and not RANGES[key][0] <= value[key] <= RANGES[key][1]:
            raise ValueError(f'{key} 必须在 {RANGES[key][0]}～{RANGES[key][1]} 之间')
    return value


def read():
    try:
        return validate(json.loads(POLICY_FILE.read_text()))
    except FileNotFoundError:
        return dict(DEFAULTS)
    # Invalid policy is reported, never silently replaced with permission to reboot.


def revision(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def app_revision(value):
    return revision({key: value[key] for key in APP_KEYS})


def write(value, expected_revision=None):
    value = validate(value)
    POLICY_FILE.parent.mkdir(parents=True, exist_ok=True)
    with POLICY_FILE.with_suffix('.lock').open('a+') as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        current = read()
        if expected_revision is not None and expected_revision != revision(current):
            raise PolicyConflict('策略已被其他页面修改，请刷新后再保存')
        temporary = POLICY_FILE.with_suffix('.tmp')
        with temporary.open('w') as output:
            json.dump(value, output, ensure_ascii=False, indent=2)
            output.write('\n')
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, POLICY_FILE)
        directory = os.open(POLICY_FILE.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    return value
