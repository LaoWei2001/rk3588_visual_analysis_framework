#!/usr/bin/python3 -I
"""Installed as the executable package entry point. Tool commands also work offline."""
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import sys


DEFAULT_APPS_ROOT = '/opt/ai_apps'


def select_config(app, argument):
    assets = app / 'assets'
    if argument is None:
        previous = app / 'run.config'
        choices = [previous.read_text().strip()] if previous.is_file() else []
        choices.append('config.json')
        choices.extend(p.name for p in sorted(assets.glob('*.json')))
        for name in choices:
            if Path(name).name == name and name.endswith('.json') and (assets / name).is_file():
                return name
        raise ValueError('assets/ 中没有可用的 JSON 配置')
    supplied = Path(argument)
    if len(supplied.parts) == 1:
        supplied = assets / supplied
    elif supplied.parts[0] == 'assets':
        supplied = app / supplied
    path = supplied.resolve()
    if path.parent != assets.resolve() or path.suffix != '.json' or not path.is_file():
        raise ValueError('请选择当前程序 assets/ 下已存在的 JSON 配置')
    return path.name


def in_previous_service(app):
    # Older transient units may still name the public entry after a package migration.
    if not os.environ.get('INVOCATION_ID'):
        return False
    import re
    slug = re.sub(r'[^A-Za-z0-9_.-]+', '-', app.name).strip('.-')[:40] or 'app'
    key = hashlib.sha256(app.name.encode()).hexdigest()[:12]
    unit = f'rk3588-app-{slug}-{key}.service'
    return any(unit in line.split(':')[-1].split('/') for line in Path('/proc/self/cgroup').read_text().splitlines())


def start_without_console(app, config):
    # Use the installed platform helper, never elevate code from the application directory.
    helper = '/usr/lib/vision-analysis-platform/python/rk3588_runtime/manual_start.py'
    if not Path(helper).is_file():
        raise ValueError('请先安装设备运行平台')
    args = ['/usr/bin/python3', '-I', helper, str(app), config]
    if os.geteuid() != 0:
        args = ['/usr/bin/sudo', '--', *args]
    env = {'PATH': '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin', 'LANG': 'C.UTF-8'}
    os.execve(args[0], args, env)


def display(message):
    if 'output' in message:
        print(message['output'], flush=True)
    elif message.get('ok'):
        print(f"{message['app']} 已启动，配置 {message['config']}，PID {message['pid']}。按 Ctrl+C 停止。", flush=True)
        for warning in message.get('service_sync', {}).get('errors', []):
            print(warning, file=sys.stderr)
    elif 'detail' in message:
        raise ValueError(message['detail'] or '启动失败')


def main():
    entry = Path(__file__).resolve()
    app = entry.parent
    args = sys.argv[1:]
    # An unpacked development build runs in the foreground. Installed packages
    # submit a start request, except for the engine's synchronous tool commands.
    installed_roots = {Path(DEFAULT_APPS_ROOT), Path(os.environ.get('APPS_ROOT', DEFAULT_APPS_ROOT)).resolve()}
    if args and args[0] in ('--help', '-h'):
        print(f"""用法
  ./{entry.name} [./assets/配置.json]
  ./{entry.name} <工具选项>
""")
        if app.parent in installed_roots:
            print("省略配置时优先使用上次配置，其次使用 assets/config.json，最后按文件名选择首个 JSON。")
            print("重复启动同一程序会重启它。终端实时显示输出，按 Ctrl+C 停止本次运行。")
        else:
            print("默认配置为 ./assets/config.json。程序在前台运行，按 Ctrl+C 停止。")
        print("""
工具选项
  -h, --help                         显示本帮助
  --validate-config <配置.json>      校验视频配置及其引用的文件
  --list-logics                      列出通道级业务逻辑
  --list-global-logics               列出全局业务逻辑
  --inspect-model <模型.rknn> [类型] 查看模型输入输出，可指定模型类型检查兼容性
  --image-inference-capabilities     查看图片推理支持的模型类型和限制
  --infer-images <任务.json>         执行图片推理任务
  --watchdog-protocol                显示引擎支持的看门狗协议""")
        return
    if app.parent not in installed_roots or (args and args[0].startswith('-')) or in_previous_service(app):
        env = os.environ.copy()
        previous = env.get('LD_LIBRARY_PATH', '')
        env['LD_LIBRARY_PATH'] = str(app / 'libs') + (':' + previous if previous else '')
        env['ASSETS_DIR'] = str(app / 'assets')
        engine = str(entry) + '.bin'
        os.execve(engine, [engine, *args], env)

    if len(args) > 1:
        raise ValueError('用法：./vision_analysis [./assets/配置.json]')
    config = select_config(app, args[0] if args else None)
    request = json.dumps({'app': app.name, 'config': config, 'foreground': True}).encode() + b'\n'
    with socket.socket(socket.AF_UNIX) as client:
        # Only the connection is time-limited. A managed restart waits for clean shutdown.
        client.settimeout(5)
        try:
            client.connect(os.environ.get('RK_CONSOLE_START_SOCKET', '/run/rk3588-console-start.sock'))
        except PermissionError:
            raise ValueError('当前账号没有本机托管启动权限，请使用 sudo 或从 Web 登录启动')
        except (FileNotFoundError, ConnectionRefusedError):
            start_without_console(app, config)
        client.settimeout(None)
        sent = False
        interrupted = False

        def request_stop(*_):
            nonlocal interrupted
            interrupted = True
            if sent:
                try:
                    client.shutdown(socket.SHUT_WR)
                except OSError:
                    pass

        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(sig, request_stop)
        client.sendall(request)
        sent = True
        if interrupted:
            request_stop()
        with client.makefile('rb') as reader:
            for line in reader:
                message = json.loads(line)
                if 'exit' in message:
                    return message['exit']
                display(message)
        raise ValueError('终端日志连接已断开，请在 Web 查看程序状态')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        sys.exit(str(error))
