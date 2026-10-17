#!/usr/bin/env python3
"""Install the system runtime platform, separately from the Web console."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
PREFIX = Path('/usr/lib/vision-analysis-platform')
UNIT_DIR = Path('/lib/systemd/system')


def env_value(value):
    return str(value).replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')


def stage(root, apps_root=None):
    destination = root / PREFIX.relative_to('/')
    units = root / UNIT_DIR.relative_to('/')
    destination.mkdir(parents=True, exist_ok=True)
    units.mkdir(parents=True, exist_ok=True)
    package = HERE / 'python/rk3588_runtime'
    shutil.copytree(package, destination / 'python/rk3588_runtime', dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
    watchdog = HERE.parent / 'system_watchdog'
    (destination / 'watchdog').mkdir(exist_ok=True)
    for name in ('install_watchdog.py', 'recovery_monitor.py'):
        shutil.copy2(watchdog / name, destination / 'watchdog' / name)
    shutil.copy2(Path(__file__), destination / 'install_runtime.py')
    apps_root = apps_root or os.environ.get('APPS_ROOT', '/opt/ai_apps')
    if not Path(apps_root).is_absolute():
        raise ValueError('APPS_ROOT 必须是绝对路径')
    shared_env = f'Environment="PYTHONPATH={PREFIX}/python"\n'
    (units / 'rk3588-runtime-manager.service').write_text(
        '[Unit]\nDescription=RK3588 independent device runtime manager\n'
        'After=local-fs.target network.target\nBefore=rk3588-console.service\n'
        'StartLimitIntervalSec=0\n\n[Service]\nType=exec\nUser=root\n' + shared_env +
        f'Environment="APPS_ROOT={env_value(apps_root)}"\n'
        'Environment="BINARY_NAME=vision_analysis"\nEnvironment="VISION_PYTHON=/usr/bin/python3"\n'
        'ExecStart=/usr/bin/python3 -m rk3588_runtime.manager\n'
        'Restart=always\nRestartSec=5\nWatchdogSec=120\nWatchdogSignal=SIGKILL\nNotifyAccess=main\n'
        'RuntimeDirectory=rk3588-runtime\nRuntimeDirectoryPreserve=restart\nTimeoutStopSec=10\n'
        'StandardOutput=journal\nStandardError=journal\n\n[Install]\nWantedBy=multi-user.target\n')
    (units / 'rk3588-recovery.service').write_text(
        '[Unit]\nDescription=RK3588 kernel-blocked application recovery\nAfter=local-fs.target\n\n'
        '[Service]\nType=oneshot\n' + shared_env +
        f'ExecStart=/usr/bin/python3 {PREFIX}/watchdog/recovery_monitor.py\nTimeoutStartSec=25\n')
    (units / 'rk3588-recovery.timer').write_text(
        '[Unit]\nDescription=RK3588 independent application recovery check\n\n'
        '[Timer]\nOnBootSec=90\nOnUnitActiveSec=30\nAccuracySec=1s\n\n'
        '[Install]\nWantedBy=timers.target\n')
    (units / 'rk3588-network-recovery.service').write_text(
        '[Unit]\nDescription=RK3588 independent network recovery\n'
        'After=local-fs.target network.target NetworkManager.service\nStartLimitIntervalSec=0\n\n'
        '[Service]\nType=exec\nUser=root\n' + shared_env +
        f'Environment="APPS_ROOT={env_value(apps_root)}"\n'
        'ExecStart=/usr/bin/python3 -m rk3588_runtime.network_recovery\n'
        'Restart=always\nRestartSec=5\nWatchdogSec=120\nWatchdogSignal=SIGKILL\nNotifyAccess=main\n'
        'RuntimeDirectory=rk3588-network-recovery\nStateDirectory=rk3588-network-recovery\n'
        'TimeoutStopSec=10\nStandardOutput=journal\nStandardError=journal\n\n'
        '[Install]\nWantedBy=multi-user.target\n')
    (units / 'rk3588-recovery-history.service').write_text(
        '[Unit]\nDescription=RK3588 bounded recovery history and diagnostics\n'
        'After=local-fs.target systemd-journald.service systemd-pstore.service\n'
        'Before=rk3588-runtime-manager.service\nStartLimitIntervalSec=0\n\n'
        '[Service]\nType=exec\nUser=root\n' + shared_env +
        f'Environment="APPS_ROOT={env_value(apps_root)}"\n'
        'ExecStart=/usr/bin/python3 -m rk3588_runtime.recovery_observer\n'
        'ExecStopPost=/usr/bin/python3 -m rk3588_runtime.recovery_observer --shutdown\n'
        'Restart=always\nRestartSec=5\nWatchdogSec=120\nWatchdogSignal=SIGKILL\nNotifyAccess=main\n'
        'RuntimeDirectory=rk3588-recovery-history\nStateDirectory=rk3588-runtime\n'
        'TimeoutStopSec=15\nStandardOutput=journal\nStandardError=journal\n\n'
        '[Install]\nWantedBy=multi-user.target\n')
    (units / 'rk3588-performance.service').write_text(
        '[Unit]\nDescription=RK3588 saved performance limits\n'
        'After=local-fs.target systemd-modules-load.service\nBefore=rk3588-console.service rk3588-runtime-manager.service\n\n'
        '[Service]\nType=oneshot\nUser=root\n' + shared_env +
        'ExecStart=/usr/bin/python3 -m rk3588_runtime.performance\n'
        'TimeoutStartSec=20\nRemainAfterExit=yes\nStandardOutput=journal\nStandardError=journal\n\n'
        '[Install]\nWantedBy=multi-user.target\n')
    # Bound journals regardless of whether the board uses volatile or persistent
    # storage. Deliberately preserve the administrator's Storage= choice.
    journald = root / 'etc/systemd/journald.conf.d'
    journald.mkdir(parents=True, exist_ok=True)
    (journald / 'zz-rk3588-bounded-logs.conf').write_text(
        '# Managed by vision-analysis-platform\n[Journal]\n'
        'SystemMaxUse=64M\nSystemMaxFileSize=8M\nSystemMaxFiles=8\n'
        'RuntimeMaxUse=32M\nRuntimeMaxFileSize=8M\nRuntimeMaxFiles=4\nMaxRetentionSec=7day\n')
    coredump = root / 'etc/systemd/coredump.conf.d'
    coredump.mkdir(parents=True, exist_ok=True)
    (coredump / 'zz-rk3588-bounded-dumps.conf').write_text(
        '# Managed by vision-analysis-platform\n[Coredump]\n'
        'ProcessSizeMax=64M\nExternalSizeMax=16M\nMaxUse=32M\nKeepFree=512M\n')
    for unit in ('rk3588-console', 'rk3588-runtime-manager', 'rk3588-network-recovery', 'rk3588-recovery-history',
                 'ota_agent', 'unified_upload', 'rk3588-gpio-control', 'rk3588-gpio-restore'):
        dropin = units / (unit + '.service.d')
        dropin.mkdir(exist_ok=True)
        (dropin / '90-bounded-dumps.conf').write_text('[Service]\nLimitCORE=16M\n')
        (dropin / '91-recovery-records.conf').write_text('[Service]\n'
            'ExecStopPost=-/usr/bin/env PYTHONPATH=/usr/lib/vision-analysis-platform/python /usr/bin/python3 -m rk3588_runtime.recovery_observer --service-exit %n\n'
            'ExecStartPost=-/usr/bin/env PYTHONPATH=/usr/lib/vision-analysis-platform/python /usr/bin/python3 -m rk3588_runtime.recovery_observer --service-start %n\n')


def activate(watchdog_timeout=60):
    if os.geteuid() != 0:
        raise RuntimeError('安装系统运行平台需要 root 权限')
    # Retire only units generated by the previous project installer. Unknown
    # administrator overrides are preserved and reported instead of overwritten.
    for name in ('rk3588-recovery.service', 'rk3588-recovery.timer'):
        legacy = Path('/etc/systemd/system') / name
        if legacy.is_file():
            content = legacy.read_text()
            if 'Description=RK3588 ' not in content:
                raise RuntimeError(f'发现自定义服务覆盖配置，请先检查：{legacy}')
            backup = legacy.with_suffix(legacy.suffix + '.pre-platform')
            if not backup.exists():
                shutil.copy2(legacy, backup)
            legacy.unlink()
    spec = importlib.util.spec_from_file_location('platform_watchdog', PREFIX / 'watchdog/install_watchdog.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    print(json.dumps({'kernel_panic_recovery': module.ensure_panic_reboot()}, ensure_ascii=False, indent=2))
    print(json.dumps(module.ensure(watchdog_timeout), ensure_ascii=False, indent=2))
    upgrade_daily_reboot(Path('/etc/systemd/system/rk3588-daily-reboot.service'))
    subprocess.run(['systemctl', 'daemon-reload'], check=True, timeout=15)
    subprocess.run(['systemctl', 'enable', '--now', 'rk3588-recovery.timer', 'rk3588-runtime-manager.service',
                    'rk3588-network-recovery.service', 'rk3588-recovery-history.service', 'rk3588-performance.service'],
                   check=True, timeout=30)
    subprocess.run(['systemctl', 'restart', 'systemd-journald.service'], check=True, timeout=15)
    subprocess.run(['systemctl', 'restart', 'rk3588-runtime-manager.service'], check=True, timeout=30)
    subprocess.run(['systemctl', 'restart', 'rk3588-network-recovery.service'], check=True, timeout=30)


def upgrade_daily_reboot(path):
    if not path.is_file() or path.is_symlink():
        return False
    content = path.read_text()
    if 'Description=RK3588 Daily Scheduled Reboot' not in content or 'rk3588_runtime.reboot_coordinator --scheduled' in content:
        return False
    if '[Service]\n' not in content:
        return False
    match = re.search(r'^ExecStart=(/[^\s"\n]+) reboot\s*$', content, re.MULTILINE)
    if not match or len(re.findall(r'^ExecStart=', content, re.MULTILINE)) != 1:
        return False  # Preserve administrator commands, including custom force policies.
    systemctl = match.group(1)
    if Path(systemctl).name != 'systemctl' or '%' in systemctl:
        return False
    content = content[:match.start()] + 'ExecStart=/usr/bin/python3 -m rk3588_runtime.reboot_coordinator --scheduled\n' + content[match.end():]
    for line in ('ExecStartPre=/usr/bin/python3 -m rk3588_runtime.recovery_observer --record-scheduled-reboot',
                 'ExecStopPost=/usr/bin/python3 -m rk3588_runtime.recovery_observer --scheduled-result'):
        content = content.replace(line + '\n', '')
    additions = f'Environment="SYSTEMCTL_PATH={systemctl}"\n'
    if 'Environment=PYTHONPATH=/usr/lib/vision-analysis-platform/python' not in content:
        additions += 'Environment=PYTHONPATH=/usr/lib/vision-analysis-platform/python\n'
    if not re.search(r'^TimeoutStartSec=', content, re.MULTILINE):
        additions += 'TimeoutStartSec=90\n'
    content = content.replace('[Service]\n', '[Service]\n' + additions, 1)
    temporary = path.with_suffix('.tmp')
    temporary.write_text(content)
    temporary.replace(path)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--watchdog-timeout', type=int, default=60)
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--stage', type=Path, help='仅生成平台包文件，不操作系统服务')
    group.add_argument('--activate-only', action='store_true')
    args = parser.parse_args()
    if not 10 <= args.watchdog_timeout <= 3600:
        parser.error('--watchdog-timeout 必须在 10～3600 秒之间')
    if args.stage:
        stage(args.stage)
    else:
        if os.geteuid() != 0:
            parser.error('请使用 root 安装运行平台')
        if not args.activate_only:
            stage(Path('/'))
        activate(args.watchdog_timeout)


if __name__ == '__main__':
    main()
