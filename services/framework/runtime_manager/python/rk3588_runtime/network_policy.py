"""Network recovery configuration, independent of HTTP and application policy."""
import copy
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import re

from .recovery_policy import PolicyConflict, revision

POLICY_FILE = Path(os.environ.get('NETWORK_RECOVERY_POLICY_FILE', '/etc/rk3588-runtime/network-recovery.json'))
DEFAULTS = {
    'enabled': False,
    'interface': '',
    'targets': [],
    'failure_timeout_sec': 180,
    'recovery_wait_sec': 60,
    'action_cooldown_sec': 300,
    'allow_manager_restart': True,
    'allow_reboot': False,
    'reboot_after_sec': 900,
    'reboot_cooldown_sec': 3600,
    'max_reboots_per_day': 2,
}
RANGES = {'failure_timeout_sec': (60, 1800), 'recovery_wait_sec': (30, 600),
          'action_cooldown_sec': (60, 3600), 'reboot_after_sec': (300, 7200),
          'reboot_cooldown_sec': (1800, 86400), 'max_reboots_per_day': (1, 5)}
INTERFACE = re.compile(r'[A-Za-z0-9_.:-]{1,15}')


def clean_host(raw):
    if not isinstance(raw, str) or not raw or raw != raw.strip() or len(raw) > 253:
        raise ValueError('探测地址必须为 IP 或主机名，不含协议、路径或空格')
    try:
        address = ipaddress.ip_address(raw)
    except ValueError:
        try:
            host = raw.rstrip('.').encode('idna').decode('ascii').lower()
        except UnicodeError as exc:
            raise ValueError('探测主机名不正确') from exc
        if host == 'localhost' or host.endswith('.localhost') or not all(
                re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?', part) for part in host.split('.')):
            raise ValueError('探测主机名不正确，请勿填写 URL 或本机回环地址')
        return host
    if address.is_loopback or address.is_unspecified or address.is_multicast or address.is_link_local:
        raise ValueError('探测地址不能使用回环、未指定、组播或链路本地地址')
    return str(address)


def validate(raw):
    if not isinstance(raw, dict) or set(raw) - set(DEFAULTS):
        raise ValueError('网络恢复策略包含未知字段或格式错误')
    value = {**copy.deepcopy(DEFAULTS), **raw}
    for key, default in DEFAULTS.items():
        if type(value[key]) is not type(default):
            raise ValueError(f'{key} 的类型不正确')
        if key in RANGES and not RANGES[key][0] <= value[key] <= RANGES[key][1]:
            raise ValueError(f'{key} 必须在 {RANGES[key][0]}～{RANGES[key][1]} 之间')
    if value['interface'] and (not INTERFACE.fullmatch(value['interface']) or value['interface'] == 'lo'):
        raise ValueError('请选择有效的有线或 Wi-Fi 管理网卡')
    if len(value['targets']) > 4:
        raise ValueError('最多设置 4 个探测目标')
    targets = []
    for target in value['targets']:
        if not isinstance(target, dict):
            raise ValueError('探测目标格式不正确')
        method = target.get('method', 'tcp')
        if method == 'ping':
            if set(target) != {'host', 'method'}:
                raise ValueError('ping 检测只需地址和检测方式，不填写端口')
            item = {'host': clean_host(target['host']), 'method': 'ping'}
        elif method == 'tcp':
            if set(target) not in ({'host', 'port'}, {'host', 'port', 'method'}):
                raise ValueError('TCP 检测目标必须包含地址和端口')
            if type(target['port']) is not int or not 1 <= target['port'] <= 65535:
                raise ValueError('探测端口必须为 1～65535 的整数')
            # Keep legacy TCP policies byte-for-byte equivalent after validation,
            # so upgrading does not invalidate their successful-probe baseline.
            item = {'host': clean_host(target['host']), 'port': target['port']}
        else:
            raise ValueError('检测方式必须为 ping 或 tcp')
        if item not in targets:
            targets.append(item)
    value['targets'] = targets
    if value['enabled'] and not targets:
        raise ValueError('启用网络恢复前至少配置一个探测目标')
    if value['reboot_after_sec'] < value['failure_timeout_sec'] + 2 * value['recovery_wait_sec']:
        raise ValueError('整机重启期限必须留出连接恢复和网络服务恢复的等待时间')
    return value


def read():
    try:
        return validate(json.loads(POLICY_FILE.read_text()))
    except FileNotFoundError:
        return copy.deepcopy(DEFAULTS)


def write(raw, expected_revision):
    value = validate(raw)
    POLICY_FILE.parent.mkdir(parents=True, exist_ok=True)
    with POLICY_FILE.with_suffix('.lock').open('a+') as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        if expected_revision != revision(read()):
            raise PolicyConflict('网络恢复策略已被修改，请重新读取后保存')
        temporary = POLICY_FILE.with_suffix('.tmp')
        with temporary.open('w') as output:
            json.dump(value, output, ensure_ascii=False, indent=2)
            output.write('\n'); output.flush(); os.fsync(output.fileno())
        os.replace(temporary, POLICY_FILE)
        directory = os.open(POLICY_FILE.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    return value
