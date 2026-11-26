"""User-initiated video application start, shared by the console and local operators."""
import asyncio
import os
from pathlib import Path
import sys
import signal
import shlex
import subprocess

if __name__ == '__main__':
    # Use the same installed root/binary settings as the independent device supervisor,
    # including when sudo stripped the invoking shell's environment.
    values = subprocess.check_output(['/usr/bin/systemctl', 'show', 'rk3588-runtime-manager.service',
                                      '--property=Environment', '--value'], text=True)
    for setting in shlex.split(values):
        key, _, value = setting.partition('=')
        if key in ('APPS_ROOT', 'BINARY_NAME'):
            os.environ[key] = value
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from rk3588_runtime import background_services, process_manager as pm, recovery_history


def start(name, config, source, restart=False, **launch_options):
    action = '重启' if restart else '启动'
    event_id = recovery_history.record('application', 'manual_restart' if restart else 'manual_start',
                                       f'用户通过{source}主动{action}程序', f'{action}程序 {name}', source=source)
    try:
        pid = pm.start_app(name, config, **launch_options)
        recovery_history.update(event_id, 'performed', f'程序进程已启动，PID {pid}；业务状态需另行检查。')
        return {"ok": True, "pid": pid, "service_sync": background_services.sync_services_for_running_app()}
    except Exception as exc:
        if event_id:
            recovery_history.update(event_id, 'failed', str(exc))
        raise


def offline_main(directory, config):
    if os.geteuid() != 0:
        raise ValueError('离线托管启动需要管理员权限')
    directory = Path(directory).resolve()
    if directory.parent != pm.APPS_ROOT.resolve():
        raise ValueError('程序目录与已安装平台的 APPS_ROOT 不一致')
    app = directory.name
    from rk3588_runtime import foreground
    from rk3588_runtime.app_entry import display

    async def run():
        stop = asyncio.Event()
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            loop.add_signal_handler(sig, stop.set)

        async def output(message):
            display(message)

        return await foreground.run(app, config, start, output, stop.wait)

    try:
        return asyncio.run(run())
    finally:
        process = pm._processes.get(app)
        if process and process.launcher and process.launcher.poll() is None:
            process.launcher.terminate()
            process.launcher.wait()


if __name__ == '__main__':
    try:
        sys.exit(offline_main(*sys.argv[1:]))
    except (TypeError, ValueError, RuntimeError, OSError) as error:
        sys.exit(str(error))
