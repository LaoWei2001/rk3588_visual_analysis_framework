"""Relay one managed launch to its terminal, including startup and shutdown output."""
import asyncio
import json
import uuid

from . import process_manager as pm, recovery_history, runtime_state


def _stop_current(app, unit, tag):
    # A delayed Ctrl+C from the old terminal must never stop a replacement launch.
    with pm.runtime_lock():
        if pm._systemd_unit_status(unit).get('LogExtraFields') != tag:
            return
        result = pm.stop_app_detailed(app)
        recovery_history.safe_record('application', 'manual_stop', '前台终端请求停止程序',
                                     f'停止程序 {app}', 'performed', result['message'], source='本机命令行')


async def _wait_for_exit(app, unit, tag, pid):
    ticks = pm._process_start_ticks(pid)
    while True:
        await asyncio.sleep(.25)
        if ticks is not None and pm._process_start_ticks(pid) == ticks:
            continue
        state = await asyncio.to_thread(pm._systemd_unit_status, unit)
        if state.get('LogExtraFields') != tag or state.get('ActiveState') in ('inactive', 'failed'):
            record = pm._shutdown_record(app, pid, ticks)
            if type(record.get('exit_code')) is int:
                return record['exit_code']
            if state.get('LogExtraFields') == tag and state.get('ExecMainCode') in ('1', 'exited'):
                return int(state['ExecMainStatus'])
            # Collected legacy units may have no exit record. A requested stop or
            # replacement completes the old terminal, an unexpected loss reports failure.
            replaced = state.get('MainPID', '0') != '0'
            stopped = not runtime_state.get_vision_settings(app)['desired_running']
            return 0 if replaced or stopped else 1
        next_pid = int(state.get('MainPID') or 0)
        if next_pid and next_pid != pid:
            pid, ticks = next_pid, pm._process_start_ticks(next_pid)


class RunJournal:
    def __init__(self, unit, tag):
        self.command = ['journalctl', '--boot', '--quiet', '--no-pager', '--all', '--output=json',
                        '--output-fields=MESSAGE', '_SYSTEMD_UNIT=' + unit, tag]
        self.cursor = None

    async def copy(self, emit, *, follow=False):
        args = list(self.command)
        if follow:
            args.extend(('--follow', '--no-tail'))
        if self.cursor:
            args.append('--after-cursor=' + self.cursor)
        proc = await asyncio.create_subprocess_exec(*args, stdout=asyncio.subprocess.PIPE,
                                                    limit=8 * 1024 * 1024)
        try:
            async for line in proc.stdout:
                entry = json.loads(line)
                message = entry.get('MESSAGE', '')
                if isinstance(message, list):
                    message = bytes(message).decode(errors='replace')
                await emit({'output': message})
                self.cursor = entry['__CURSOR']
            if await proc.wait():
                raise RuntimeError('读取本次运行日志失败')
        finally:
            if proc.returncode is None:
                try:
                    proc.terminate()
                except ProcessLookupError:
                    pass
                await proc.communicate()

    async def finish(self, follower, emit):
        follower.cancel()
        await asyncio.gather(follower, return_exceptions=True)
        # Drain entries journaled while the terminal or engine was shutting down.
        await self.copy(emit)


async def run(app, config, start, emit, wait_stop):
    token = uuid.uuid4().hex
    tag = 'RK_APP_RUN_ID=' + token
    unit = pm._app_unit_name(app)
    journal = RunJournal(unit, tag)
    log = asyncio.create_task(journal.copy(emit, follow=True))
    tasks = [log]
    starting = asyncio.create_task(asyncio.to_thread(start, app, config, '本机命令行', journal_tag=token))
    try:
        try:
            result = await asyncio.shield(starting)
        except asyncio.CancelledError:
            # The thread can already be creating the service. Resolve ownership before cleanup.
            await asyncio.gather(starting, return_exceptions=True)
            raise
        except Exception:
            await journal.finish(log, emit)
            raise
        await emit({**result, 'app': app, 'config': config})
        exit_status = asyncio.create_task(_wait_for_exit(app, unit, tag, result['pid']))
        stop = asyncio.create_task(wait_stop())
        tasks.extend((exit_status, stop))
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        if stop in done:
            await asyncio.to_thread(_stop_current, app, unit, tag)
        if log in done:
            await log
            raise RuntimeError('实时日志连接已结束')
        code = await exit_status
        await journal.finish(log, emit)
        return code
    except BaseException:
        await asyncio.to_thread(_stop_current, app, unit, tag)
        raise
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
