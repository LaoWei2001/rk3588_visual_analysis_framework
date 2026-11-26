import asyncio
import os
import shutil
import signal
import threading

from services import local_start
from services.runtime_loader import load_runtime
from routers import process


def test_installed_entry_and_web_use_same_start(tmp_path, monkeypatch):
    installer = load_runtime('package_install')
    root = tmp_path / 'apps'
    app = root / 'demo'
    (app / 'assets').mkdir(parents=True)
    (app / 'assets/live.json').write_text('{}')
    (app / 'run.config').write_text('live.json')
    (app / 'run.sh').write_text('old launcher')
    entry = app / 'vision_analysis'
    shutil.copy2('/bin/echo', entry)
    installer.prepare_entrypoint(app, entry.name, root)
    installer.prepare_entrypoint(app, entry.name, root)
    assert not (app / 'run.sh').exists()
    calls = []
    monkeypatch.setattr(process.pm, 'start_app', lambda app, config, **kw: calls.append((app, config)) or 123)
    session = local_start.foreground
    stop = threading.Event()
    hold = False

    async def journal(self, emit, *, follow=False):
        if follow:
            await emit({'output': 'engine stdout'})
            await asyncio.Future()
        await emit({'output': 'shutdown complete'})

    async def wait_exit(*args):
        if hold:
            assert await asyncio.to_thread(stop.wait, 5)
        return 0

    monkeypatch.setattr(session.RunJournal, 'copy', journal)
    monkeypatch.setattr(session, '_wait_for_exit', wait_exit)
    monkeypatch.setattr(session, '_stop_current', lambda *args: stop.set())
    monkeypatch.setattr(process.background_services, 'sync_services_for_running_app', lambda: {'errors': []})

    async def run():
        nonlocal hold
        path = tmp_path / 'start.sock'
        server = await local_start.listen(path, process.manual_start)
        async def spawn(*args):
            return await asyncio.create_subprocess_exec(str(entry), *args,
                env={**os.environ, 'RK_CONSOLE_START_SOCKET': str(path)},
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)

        async def invoke(*args):
            child = await spawn(*args)
            out, err = await child.communicate()
            return child.returncode, (out + err).decode()

        try:
            code, usage = await invoke('--help')
            assert code == 0 and '--validate-config' in usage and '--watchdog-protocol' in usage
            assert await invoke('-h') == (0, usage)
            assert calls == []
            for args in ([], ['live.json'], ['./assets/live.json'], [str(app / 'assets/live.json')]):
                code, message = await invoke(*args)
                assert code == 0 and 'PID 123' in message
            assert (await invoke('--validate-config', './assets/live.json')) == (0, '--validate-config ./assets/live.json\n')
            assert (await invoke('../other.json'))[0] != 0
            assert (await process.start_app('demo', process.StartRequest(config='live.json')))['pid'] == 123
            for stop_from_terminal in (True, False):
                stop.clear()
                hold = True
                child = await spawn('live.json')
                async def until_running():
                    text = b''
                    while b'PID 123' not in text or b'engine stdout' not in text:
                        line = await child.stdout.readline()
                        assert line
                        text += line
                await asyncio.wait_for(until_running(), 5)
                assert child.returncode is None
                if stop_from_terminal:
                    child.send_signal(signal.SIGINT)
                else:
                    stop.set()  # The engine exited after a Web stop.
                tail, err = await asyncio.wait_for(child.communicate(), 5)
                assert child.returncode == 0 and b'shutdown complete' in tail, err
            assert calls == [('demo', 'live.json')] * 7
        finally:
            server.close()
            await server.wait_closed()

    asyncio.run(run())
    assert (app / 'assets/live.json').read_text() == '{}'


def test_stale_terminal_cannot_stop_replacement(tmp_path, monkeypatch):
    fg = local_start.foreground
    monkeypatch.setattr(fg.pm, 'APPS_ROOT', tmp_path)
    monkeypatch.setattr(fg.pm, '_systemd_unit_status', lambda unit: {'LogExtraFields': 'RK_APP_RUN_ID=new'})
    stops = []
    monkeypatch.setattr(fg.pm, 'stop_app_detailed', lambda name: stops.append(name))
    fg._stop_current('demo', 'demo.service', 'RK_APP_RUN_ID=old')
    assert stops == []
