#!/usr/bin/env python3
"""Configure the systemd hardware watchdog; inspecting devices never opens them."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


CONFIG = Path('/etc/systemd/system.conf.d/90-rk3588-watchdog.conf')
PANIC_CONFIG = Path('/etc/sysctl.d/90-rk3588-recovery.conf')
PANIC_TIMEOUT = Path('/proc/sys/kernel/panic')
HARDWARE_DRIVERS = {'gpio-wdt', 'dw_wdt'}
MONITOR_DIR = Path('/usr/local/lib/rk3588-recovery')
SYSTEMD_DIR = Path('/etc/systemd/system')
MANAGED_MARKER = '# Managed by services/framework/system_watchdog/install_watchdog.py'


def saved_switches():
    # The same persistent user choices apply to source and packaged installers.
    for directory in (Path(__file__).resolve().parents[1] / 'runtime_manager/python',
                      Path('/usr/lib/vision-analysis-platform/python')):
        if (directory / 'rk3588_runtime/system_recovery.py').is_file():
            if str(directory) not in sys.path:
                sys.path.insert(0, str(directory))
            from rk3588_runtime.system_recovery import read
            return read()
    return None


def atomic_write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def ensure_panic_reboot(seconds=10):
    """Recover a kernel panic too; this does not recover a hard kernel lockup."""
    if os.geteuid() != 0:
        raise RuntimeError('配置内核崩溃恢复需要 root 权限')
    if not isinstance(seconds, int) or isinstance(seconds, bool) or not 1 <= seconds <= 300:
        raise ValueError('内核崩溃重启等待必须为 1～300 秒')
    switches = saved_switches()
    if switches and not switches['panic_enabled']:
        return {'enabled': False, 'changed': False, 'warning': '用户已关闭系统崩溃自动重启，保留设置'}
    original = int(PANIC_TIMEOUT.read_text().strip())
    # A nonzero administrator setting already requests reboot (negative means
    # immediate). Keep it rather than making an existing recovery path slower.
    effective = original if original != 0 else switches['panic_timeout_sec'] if switches else seconds
    previous = PANIC_CONFIG.read_bytes() if PANIC_CONFIG.exists() else None
    content = ('# Managed by vision-analysis-platform: kernel panic recovery\n'
               '# Hard lockups still require a working hardware watchdog.\n'
               f'kernel.panic={effective}\n').encode()
    atomic_write(PANIC_CONFIG, content)
    try:
        PANIC_TIMEOUT.write_text(str(effective) + '\n')
        if int(PANIC_TIMEOUT.read_text().strip()) != effective:
            raise RuntimeError('内核未接受崩溃重启配置')
    except Exception:
        if previous is None:
            PANIC_CONFIG.unlink(missing_ok=True)
        else:
            atomic_write(PANIC_CONFIG, previous)
        raise
    return {'enabled': True, 'timeout_sec': effective, 'config_file': str(PANIC_CONFIG)}


def write_config(entry, seconds):
    content = (MANAGED_MARKER + '\n'
               f'# Detected driver: {entry["driver"]}; verify this binding after BSP updates.\n'
               '# Device ownership is not proof of successful hardware reset.\n'
               '[Manager]\n'
               f'WatchdogDevice={entry["device"]}\n'
               f'RuntimeWatchdogSec={seconds}s\n'
               f'RebootWatchdogSec={seconds}s\n')
    atomic_write(CONFIG, content.encode())


def install_monitor():
    if os.geteuid() != 0:
        raise RuntimeError('安装独立故障恢复监测需要 root 权限')
    MONITOR_DIR.mkdir(parents=True, exist_ok=True)
    temporary = MONITOR_DIR / 'recovery_monitor.py.tmp'
    shutil.copyfile(Path(__file__).with_name('recovery_monitor.py'), temporary)
    temporary.replace(MONITOR_DIR / 'recovery_monitor.py')
    SYSTEMD_DIR.mkdir(parents=True, exist_ok=True)
    (SYSTEMD_DIR / 'rk3588-recovery.service').write_text(
        '[Unit]\nDescription=RK3588 kernel-blocked application recovery\n'
        'After=local-fs.target\n\n[Service]\nType=oneshot\n'
        f'ExecStart=/usr/bin/python3 {MONITOR_DIR}/recovery_monitor.py\n'
        'TimeoutStartSec=25\n', encoding='utf-8')
    (SYSTEMD_DIR / 'rk3588-recovery.timer').write_text(
        '[Unit]\nDescription=RK3588 independent application recovery check\n\n'
        '[Timer]\nOnBootSec=90\nOnUnitActiveSec=30\nAccuracySec=1s\n\n'
        '[Install]\nWantedBy=timers.target\n', encoding='utf-8')
    subprocess.run(['systemctl', 'daemon-reload'], check=True, timeout=15)
    subprocess.run(['systemctl', 'enable', '--now', 'rk3588-recovery.timer'], check=True, timeout=15)


def devices():
    found = []
    for path in sorted(Path('/sys/class/watchdog').glob('watchdog[0-9]*')):
        device = Path('/dev') / path.name
        driver = path / 'device/driver'
        if device.is_char_device() and driver.is_symlink():
            found.append({'device': str(device), 'driver': driver.resolve().name})
    return found


def owners(device):
    found = []
    target = Path(device).resolve()
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            for fd in (process / 'fd').iterdir():
                if fd.resolve() == target:
                    found.append(int(process.name))
                    break
        except (OSError, PermissionError):
            continue
    return sorted(found)


def manager_status():
    result = subprocess.run([
        'systemctl', 'show', '--property=RuntimeWatchdogUSec',
        '--property=RebootWatchdogUSec',
    ], check=True, capture_output=True, text=True, timeout=15)
    return dict(line.split('=', 1) for line in result.stdout.splitlines() if '=' in line)


def status():
    result = {**manager_status(), 'devices': [dict(item, owners=owners(item['device'])) for item in devices()]}
    try:
        matches = re.findall(r'^\s*WatchdogDevice\s*=\s*(/dev/watchdog[0-9]+)\s*$',
                             CONFIG.read_text(), re.MULTILINE)
    except OSError:
        matches = []
    configured = matches[-1] if matches else None
    current = next((item['device'] for item in result['devices'] if item['owners'] == [1]), None)
    switches = saved_switches()
    actual_enabled = bool(current and result.get('RuntimeWatchdogUSec') not in (None, '', '0', 'infinity'))
    pending = bool(current and configured and current != configured)
    if switches:
        configured = switches['watchdog_device'] if switches['hardware_enabled'] else None
        pending = actual_enabled != switches['hardware_enabled'] or bool(
            switches['hardware_enabled'] and configured and current != configured)
    result.update(configured_device=configured, reboot_required=pending,
                  reset_verification='设备接管和心跳不等于已通过硬件复位验收')
    try:
        result['kernel_panic_timeout_sec'] = int(PANIC_TIMEOUT.read_text().strip())
    except (OSError, ValueError):
        result['kernel_panic_timeout_sec'] = None
    return result


def apply(device, seconds):
    if os.geteuid() != 0:
        raise RuntimeError('配置硬件看门狗需要 root 权限')
    if not isinstance(seconds, int) or isinstance(seconds, bool) or not 10 <= seconds <= 3600:
        raise ValueError('超时必须为 10～3600 秒')
    if not re.fullmatch(r'/dev/watchdog[0-9]+', device):
        raise ValueError('设备必须是 /dev/watchdogN')
    available = devices()
    entry = next((item for item in available if item['device'] == device), None)
    if not entry or entry['driver'] not in HARDWARE_DRIVERS:
        raise RuntimeError('没有识别到受支持的 gpio-wdt/dw_wdt 硬件看门狗')
    occupied = owners(device)
    if occupied and occupied != [1]:
        raise RuntimeError(f'{device} 已被进程 {occupied} 占用')
    if any(item['device'] != device and 1 in owners(item['device']) for item in available):
        raise RuntimeError('systemd 正在使用另一个看门狗；需要单独安排设备切换，避免旧设备意外复位')

    previous = CONFIG.read_bytes() if CONFIG.exists() else None
    write_config(entry, seconds)
    try:
        subprocess.run(['systemctl', 'daemon-reexec'], check=True, timeout=30)
        for _ in range(20):
            status = manager_status()
            if owners(device) == [1] and status.get('RuntimeWatchdogUSec') not in (None, '', '0'):
                break
            time.sleep(.5)
        else:
            raise RuntimeError('systemd 未成功持有看门狗设备')
        # GPIO watchdog reports the logical timeout unchanged. DW hardware may
        # round the timeout; report it instead of promising the requested value.
        return {'config_file': str(CONFIG), 'requested_timeout_sec': seconds,
                **entry, 'owners': owners(device), **status}
    except Exception:
        if previous is None:
            CONFIG.unlink(missing_ok=True)
        else:
            atomic_write(CONFIG, previous)
        subprocess.run(['systemctl', 'daemon-reexec'], timeout=30, check=False)
        raise


def ensure(seconds=60):
    """Prefer the vendor demo's DW driver; migrate live GPIO only on next boot.

    Closing a live watchdog without first disarming it can reset the device.
    Never change watchdog devices during daemon-reexec. The existing device
    keeps being fed until a normal reboot loads the staged configuration.
    """
    if not isinstance(seconds, int) or isinstance(seconds, bool) or not 10 <= seconds <= 3600:
        raise ValueError('超时必须为 10～3600 秒')
    switches = saved_switches()
    if switches:
        status = manager_status()
        enabled = status.get('RuntimeWatchdogUSec') not in (None, '', '0', 'infinity') and any(
            item['driver'] in HARDWARE_DRIVERS and owners(item['device']) == [1] for item in devices())
        return {'enabled': enabled, 'requested_enabled': switches['hardware_enabled'], 'changed': False,
                'reboot_required': enabled != switches['hardware_enabled'],
                'warning': '保留用户保存的整机卡死自动重启设置；硬件开关变化需正常重启设备生效，升级时不在线切换'}
    hardware = [item for item in devices() if item['driver'] in HARDWARE_DRIVERS]
    hardware.sort(key=lambda item: (item['driver'] != 'dw_wdt', item['device']))
    status = manager_status()
    active = [item for item in hardware if owners(item['device']) == [1]]
    if active and status.get('RuntimeWatchdogUSec') not in (None, '', '0'):
        current = active[0]
        preferred = hardware[0]
        if current['driver'] == 'gpio-wdt' and preferred['driver'] == 'dw_wdt':
            existing = CONFIG.read_text() if CONFIG.exists() else ''
            if MANAGED_MARKER not in existing:
                return {'enabled': True, 'changed': False, **current, **status,
                        'warning': '当前使用 GPIO 看门狗，建议按官方例程改用 dw_wdt；自定义配置未覆盖'}
            occupied = owners(preferred['device'])
            if occupied:
                raise RuntimeError(f'{preferred["device"]} 已被进程 {occupied} 占用，未安排看门狗切换')
            write_config(preferred, seconds)
            return {'enabled': True, 'changed': existing != CONFIG.read_text(),
                    **current, **status, 'configured_device': preferred['device'],
                    'configured_driver': preferred['driver'], 'reboot_required': True,
                    'warning': '已为下次启动配置官方例程使用的 dw_wdt；当前 GPIO 继续喂狗，需正常重启完成切换'}
        return {'enabled': True, 'changed': False, **active[0], **status}
    if not hardware:
        return {'enabled': False, 'changed': False,
                'warning': '没有发现可接管的硬件看门狗，整机卡死保护未启用；请在 Web「故障恢复 → 系统故障恢复」查看设备树、驱动及设备状态。网络和程序恢复可独立运行'}
    if active:
        return {'enabled': True, 'changed': True, **apply(active[0]['device'], seconds)}
    # The official watchdog_demo uses DW, currently /dev/watchdog1. Never rely
    # on device numbering or infer external reset wiring from a GPIO driver.
    if hardware[0]['driver'] == 'dw_wdt' and owners(hardware[0]['device']):
        raise RuntimeError(f'{hardware[0]["device"]} 已被其他进程占用，未回退到未经复位验收的 GPIO 看门狗')
    selected = next((item for item in hardware if not owners(item['device'])), None)
    if selected is None:
        raise RuntimeError('硬件看门狗被其他进程占用，未切换设备；请检查现有喂狗服务')
    return {'enabled': True, 'changed': True, **apply(selected['device'], seconds)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('status', help='只读检查，绝不打开 /dev/watchdog')
    automatic = commands.add_parser('ensure', help='安装时保留已有硬件保护，未开启时自动选择硬件')
    automatic.add_argument('--timeout', default=60, type=int)
    installation = commands.add_parser('install', help='接入硬件看门狗并安装独立的内核阻塞恢复监测')
    installation.add_argument('--timeout', default=60, type=int)
    configure = commands.add_parser('apply', help='持久配置并让 systemd 接管硬件看门狗')
    configure.add_argument('--device', required=True)
    configure.add_argument('--timeout', required=True, type=int)
    panic = commands.add_parser('panic-recovery', help='持久启用内核 panic 后自动重启；不制造故障')
    panic.add_argument('--timeout', default=10, type=int)
    args = parser.parse_args()
    if args.command == 'status':
        result = status()
    elif args.command == 'panic-recovery':
        result = ensure_panic_reboot(args.timeout)
    elif args.command == 'install':
        source = Path(__file__).resolve().parents[1] / 'runtime_manager/install_runtime.py'
        packaged = Path('/usr/lib/vision-analysis-platform/install_runtime.py')
        helper = source if source.is_file() else packaged
        if not helper.is_file():
            raise RuntimeError('请先安装 runtime_manager 平台组件')
        command = [sys.executable, str(helper), '--watchdog-timeout', str(args.timeout)]
        if helper == packaged:
            command.append('--activate-only')
        subprocess.run(command, check=True, timeout=120)
        result = {'runtime_manager': 'rk3588-runtime-manager.service',
                  'recovery_timer': 'rk3588-recovery.timer'}
    elif args.command == 'ensure':
        result = ensure(args.timeout)
    else:
        if not 10 <= args.timeout <= 3600:
            parser.error('--timeout 必须为 10～3600 秒')
        result = apply(args.device, args.timeout)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
