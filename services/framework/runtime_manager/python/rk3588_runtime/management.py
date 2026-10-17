"""Local management operations used by the authenticated Web adapter."""
import time
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess

from . import manager, process_manager as pm, recovery_policy, runtime_state, watchdog_probe, system_recovery, recovery_history

AGENT_UNIT = 'rk3588-runtime-manager.service'
WATCHDOG_CONFIG = Path('/etc/systemd/system.conf.d/90-rk3588-watchdog.conf')
WATCHDOG_FDS = Path('/proc/1/fd')
WATCHDOG_SYSFS = Path('/sys/class/watchdog')
WATCHDOG_DEVICES = Path('/dev')
DEVICE_TREE = Path('/sys/firmware/devicetree/base')
KERNEL_CONFIG = Path('/proc/config.gz')
BOOT_KERNEL_CONFIG = Path('/boot') / ('config-' + platform.release())
PLATFORM_DRIVERS = Path('/sys/bus/platform/drivers')
PANIC_TIMEOUT_FILE = Path('/proc/sys/kernel/panic')
WATCHDOG_VALIDATION_FILE = Path('/var/lib/rk3588-watchdog-validation/assessment.json')
MACHINE_ID_FILE = Path('/etc/machine-id')


def hardware_details(device=None, driver=None):
    details = {'driver': driver, 'configured_device': None, 'reboot_required': False,
               'panic_timeout_sec': None, 'verification_state': 'unverified',
               'verification_message': '尚未完成本设备的硬件复位验收'}
    matches = []
    for config in (WATCHDOG_CONFIG, system_recovery.WATCHDOG_OVERRIDE):
        try:
            content = config.read_text()
        except OSError:
            continue
        if re.search(r'^\s*RuntimeWatchdogSec\s*=\s*0s?\s*$', content, re.MULTILINE):
            matches = []
        else:
            matches += re.findall(r'^\s*WatchdogDevice\s*=\s*(/dev/watchdog(?:[0-9]+)?)\s*$', content, re.MULTILINE)
    if matches:
        details['configured_device'] = matches[-1]
        details['reboot_required'] = bool(device and not system_recovery._same_device(device, matches[-1]))
    try:
        details['panic_timeout_sec'] = int(PANIC_TIMEOUT_FILE.read_text().strip())
    except (OSError, ValueError):
        pass
    validation = manager.read_json(WATCHDOG_VALIDATION_FILE, {})
    if isinstance(validation, dict) and driver:
        try:
            same_device = (validation.get('machine_id') == MACHINE_ID_FILE.read_text().strip()
                           and validation.get('kernel') == platform.release()
                           and validation.get('driver') == driver)
        except OSError:
            same_device = False
        # Even a successful boot after a test can result from manual power
        # cycling. Only an explicit, scoped assessment can claim a pass.
        if same_device and validation.get('result') in ('failed', 'passed'):
            details['verification_state'] = validation['result']
            details['verification_message'] = str(validation.get('message') or
                                                  ('实机复位验收失败' if validation['result'] == 'failed' else '已通过实机复位验收'))
    return details


def unit_status(unit):
    try:
        result = subprocess.run(['systemctl', 'show', unit,
                                 '--property=LoadState,ActiveState,SubState,UnitFileState,NRestarts'],
                                capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {'installed': False, 'active': False, 'enabled': False,
                'state': 'unknown', 'sub_state': '', 'error': str(exc)}
    values = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
    return {'installed': values.get('LoadState') == 'loaded',
            'active': values.get('ActiveState') == 'active',
            'enabled': values.get('UnitFileState') in ('enabled', 'enabled-runtime'),
            'state': values.get('ActiveState', 'unknown'),
            'sub_state': values.get('SubState', ''),
            'error': result.stderr.strip() if result.returncode else None}


def hardware_status():
    probe = watchdog_probe.inspect(sysfs=WATCHDOG_SYSFS, devices=WATCHDOG_DEVICES,
                                   manager_fds=WATCHDOG_FDS, device_tree=DEVICE_TREE,
                                   kernel_config=KERNEL_CONFIG, boot_config=BOOT_KERNEL_CONFIG,
                                   drivers=PLATFORM_DRIVERS)
    values = {}
    error = None
    try:
        result = subprocess.run(['systemctl', 'show', '--property=RuntimeWatchdogUSec,RebootWatchdogUSec'],
                                capture_output=True, text=True, timeout=5)
        values = dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)
        if result.returncode:
            error = result.stderr.strip() or '系统管理器状态读取失败'
        elif 'RuntimeWatchdogUSec' not in values:
            error = '系统管理器未返回运行期间的看门狗状态'
    except (OSError, subprocess.TimeoutExpired) as exc:
        error = str(exc)
    owned = [item for item in probe['devices']
             if item['manager_owned'] and item['driver'] in watchdog_probe.HARDWARE_DRIVERS]
    current = owned[0] if owned else None
    runtime = values.get('RuntimeWatchdogUSec', '0')
    enabled = not error and runtime not in ('0', '', 'infinity') and bool(current)
    details = hardware_details(current['device'] if current else None, current['driver'] if current else None)
    description = watchdog_probe.describe(probe, enabled, error)
    if enabled and details['verification_state'] == 'failed':
        description.update(state='reset_failed', label='需检查',
                           summary='硬件看门狗已开启，但本机曾未能自动复位，请先排查后再依赖此保护。',
                           next_steps=['请结合本机测试记录检查驱动与硬件复位链路，并重新验证。'])
    return {**details, **description, 'enabled': enabled,
            'device': current['device'] if current else None,
            'runtime_timeout': runtime, 'reboot_timeout': values.get('RebootWatchdogUSec', '0'),
            'error': error or ('；'.join(probe['errors']) if probe['errors'] else None)}


def history_snapshot():
    return {**recovery_history.snapshot(),
            'observer': manager.read_json(Path('/run/rk3588-recovery-history/status.json'), {})}


def snapshot():
    policy_error = None
    try:
        policy = recovery_policy.read()
    except (OSError, ValueError) as exc:
        policy = dict(recovery_policy.DEFAULTS)
        policy_error = str(exc)
    agent = unit_status(AGENT_UNIT)
    state = manager.read_json(manager.STATUS_FILE, {})
    if not isinstance(state, dict):
        state = {}
    fresh = (agent['active'] and state.get('boot_id') == pm._current_boot_id()
             and isinstance(state.get('updated_at'), (int, float))
             and 0 <= time.time() - state['updated_at'] < 30)
    current = pm.get_running_app_context()
    app = None
    pending = False
    if current:
        app = {'app': current['app'], **pm.get_status(current['app'])}
        try:
            applied = (pm._app_path(current['app']) / 'run.recovery_policy').read_text().strip()
        except OSError:
            applied = ''
        pending = applied != recovery_policy.app_revision(policy)
        if (app.get('restart_enabled') is not None and app['restart_enabled'] != policy['app_restart_enabled']
                or app.get('watchdog_active') and not policy['app_stall_enabled']):
            pending = True
    events = manager.read_json(manager.EVENTS_FILE, [])
    if not isinstance(events, list):
        events = []
    kernel_state = manager.read_json(Path(os.environ.get('KERNEL_RECOVERY_STATE_FILE',
                                                        '/var/lib/rk3588-recovery/state.json')), {})
    if isinstance(kernel_state, dict) and isinstance(kernel_state.get('reboots'), list):
        events += [{'time': stamp, 'kind': 'board_reboot_request', 'message': '已记录因持续内核阻塞而升级整机重启的请求'}
                   for stamp in kernel_state['reboots'] if isinstance(stamp, (int, float))]
    events = [event for event in events if isinstance(event, dict) and isinstance(event.get('time'), (int, float))]
    events.sort(key=lambda event: event['time'], reverse=True)
    hardware = hardware_status()
    history = history_snapshot()
    return {'policy': policy, 'revision': recovery_policy.revision(policy),
            'policy_error': policy_error, 'agent': {**agent, 'heartbeat_fresh': fresh},
            'monitor': unit_status('rk3588-recovery.timer'),
            'hardware': hardware, 'system': system_recovery.snapshot(hardware), 'runtime': state if fresh else None,
            'current': app, 'boot_target': runtime_state.get_vision_boot_target(),
            'application_pending': pending,
            'events': events[:20], 'history': history}


def save_system_policy(value, expected_revision):
    system_recovery.save(value, expected_revision, hardware_status)
    return snapshot()


def control(action):
    commands = {'start': ['enable', '--now', AGENT_UNIT],
                'stop': ['disable', '--now', AGENT_UNIT],
                'restart': ['restart', AGENT_UNIT]}
    if action not in commands:
        raise ValueError('不支持的守护服务操作')
    event_id = recovery_history.record('system', 'manual_service_control',
        '用户请求' + {'start': '启动', 'stop': '停止', 'restart': '重启'}[action] + '独立运行协调服务',
        'systemctl ' + ' '.join(commands[action]), source='Web 故障恢复', subject=AGENT_UNIT, fault_recovery=False)
    try:
        result = subprocess.run(['systemctl', *commands[action]], capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError(result.stderr.strip() or result.stdout.strip() or '系统服务操作失败')
    except Exception as exc:
        if event_id:
            recovery_history.update(event_id, 'failed', str(exc))
        raise
    if event_id:
        recovery_history.update(event_id, 'performed', '服务控制命令已完成；实际运行状态请查看服务状态。')
    return snapshot()


def save_application_policy(value, expected_revision):
    if not isinstance(value, dict) or set(value) != set(recovery_policy.APP_KEYS):
        raise ValueError('程序恢复设置必须包含两个开关和三个等待时间，不能修改系统恢复参数')
    if value['app_stall_enabled'] and not value['app_restart_enabled']:
        raise ValueError('程序卡住后自动恢复需要同时开启程序退出后自动重启')
    current = recovery_policy.read()
    recovery_policy.write({**current, **value}, expected_revision)
    return snapshot()


def apply_to_current_app(expected_revision=None, expected_app=None):
    from . import background_services
    with pm.runtime_lock():
        if expected_revision is not None and expected_revision != recovery_policy.revision(recovery_policy.read()):
            raise recovery_policy.PolicyConflict('程序恢复设置已被修改，请重新读取后再应用')
        current = pm.get_running_app_context()
        if current is None:
            raise ValueError('没有正在运行的视觉应用；下次启动时将使用已保存策略')
        if expected_app is not None and current['app'] != expected_app:
            raise recovery_policy.PolicyConflict('当前运行程序已切换，请确认后重新应用')
        event_id = recovery_history.record('application', 'manual_restart',
            '用户要求重启程序以应用恢复设置', f"重新启动程序 {current['app']}（{current['config']}）", source='Web 故障恢复')
        try:
            pid = pm.start_app(current['app'], current['config'])
        except Exception as exc:
            recovery_history.update(event_id, 'failed', str(exc))
            raise
        recovery_history.update(event_id, 'performed', f'已重新启动程序，PID {pid}；业务状态需另行检查。')
        background_services.sync_services_for_running_app()
    return snapshot()


def network_snapshot():
    from . import network_policy, network_recovery
    policy_error = None
    try:
        settings = network_policy.read()
    except (OSError, ValueError) as exc:
        settings = network_policy.validate(network_policy.DEFAULTS)
        policy_error = str(exc)
    service = unit_status(network_recovery.UNIT)
    state = manager.read_json(network_recovery.STATUS_FILE, {})
    if not isinstance(state, dict):
        state = {}
    fresh = (service['active'] and state.get('boot_id') == pm._current_boot_id()
             and isinstance(state.get('updated_at'), (int, float))
             and 0 <= time.time() - state['updated_at'] < 60)
    persistent = manager.read_json(network_recovery.STATE_FILE, {})
    events = persistent.get('events', []) if isinstance(persistent, dict) else []
    if not isinstance(events, list):
        events = []
    interface_error = None
    missing_tools = [tool for tool in ('nmcli', 'ip', 'ping') if shutil.which(tool) is None]
    requirements = {
        'ready': not missing_tools and service['installed'], 'missing_tools': missing_tools,
        'message': ('缺少网络恢复所需工具：' + '、'.join(missing_tools)
                    + '。请管理员安装 NetworkManager、iproute2 和 ping 后再开启。') if missing_tools
                   else '请先通过项目安装器安装独立网络恢复服务。' if not service['installed'] else None,
    }
    if 'nmcli' in missing_tools:
        inventory = []
    else:
        try:
            inventory = network_recovery.interfaces()
        except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
            inventory, interface_error = [], str(exc)
    return {'policy': settings, 'revision': network_policy.revision(settings), 'policy_error': policy_error,
            'service': {**service, 'heartbeat_fresh': fresh}, 'runtime': state if fresh else None,
            'interfaces': inventory, 'interface_error': interface_error, 'requirements': requirements,
            'policy_pending': fresh and state.get('policy_revision') != network_policy.revision(settings),
            'history': {**recovery_history.snapshot('network'), 'observer': manager.read_json(Path('/run/rk3588-recovery-history/status.json'), {})},
            'events': [event for event in reversed(events) if isinstance(event, dict)
                       and isinstance(event.get('time'), (int, float)) and isinstance(event.get('message'), str)][:20]}


def save_network_policy(value, expected_revision):
    from . import network_policy, network_recovery
    network_policy.validate(value)
    service = unit_status(network_recovery.UNIT)
    if value.get('enabled') and not service['installed']:
        raise RuntimeError('请先安装独立网络恢复服务')
    if value.get('enabled') and any(shutil.which(tool) is None for tool in ('nmcli', 'ip', 'ping')):
        raise RuntimeError('启用网络恢复需要 NetworkManager/nmcli、iproute2 和 ping')
    network_policy.write(value, expected_revision)
    if value.get('enabled') and (not service['active'] or not service['enabled']):
        result = subprocess.run(['systemctl', 'enable', '--now', network_recovery.UNIT],
                                capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise RuntimeError('策略已保存，但网络恢复守护启动失败：' + result.stderr.strip())
    return network_snapshot()
