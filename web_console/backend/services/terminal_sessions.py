"""Bounded PTY sessions which can survive a short websocket transport failure."""
import asyncio
from collections import deque
from contextlib import suppress
import fcntl
import logging
import os
import pty
import pwd
import shutil
import signal
import struct
import termios
import time

from services.auth_service import get_session
from services.data_dir import APPS_ROOT

log = logging.getLogger("uvicorn.error")
RESUME_GRACE = 300
AUTH_CHECK_INTERVAL = 15
MAX_OUTPUT = 1024 * 1024
MAX_INPUT = 1024 * 1024
CHUNK_SIZE = 65536
MAX_PER_OWNER = 8
MAX_TOTAL = 32


def _shell_command(owner):
    session = get_session(owner)
    if not session:
        raise PermissionError('terminal login expired')
    try:
        account = pwd.getpwnam(session['username'])
    except (KeyError, TypeError):
        raise PermissionError('terminal system account unavailable')
    shell = account.pw_shell or '/bin/sh'
    if os.path.basename(shell) in ('nologin', 'false'):
        raise PermissionError('terminal shell is disabled for this account')
    if account.pw_uid == os.geteuid():
        return [shell, '-l']
    su = shutil.which('su')
    if os.geteuid() != 0 or not su:
        raise PermissionError('cannot launch terminal as the authenticated user')
    # Login was already verified. Here su is ONLY a privilege/session switch,
    # never a password verifier: it initializes groups and the user's environment.
    return [su, '--login', '--shell', shell, '--', account.pw_name]


class PtySession:
    def __init__(self, owner, session_id, on_close):
        command = _shell_command(owner)  # Validate the identity before forking.
        self.owner, self.id, self.on_close = owner, session_id, on_close
        self.loop = asyncio.get_running_loop()
        self.created = time.monotonic()
        self.output = deque()
        self.end = self.acked = 0
        self.changed = asyncio.Event()
        self.input_changed = asyncio.Event()
        self.input = bytearray()
        self.socket = None
        self.expiry = None
        self.eof = self.closed = self.reading = self.reaped = False
        self.exit_code = None
        self.closing = None
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["TERM"] = os.environ.get("TERM", "xterm-256color")
            os.environ.setdefault("LANG", "C.UTF-8")
            alias = "alias ls='ls --color=auto'"
            existing = os.environ.get("PROMPT_COMMAND", "")
            os.environ["PROMPT_COMMAND"] = f"{alias};{existing}" if existing else alias
            with suppress(OSError):
                os.chdir(APPS_ROOT)
            try:
                os.execlp(command[0], *command)
            except OSError:
                os._exit(127)  # Never return into the inherited Web event loop.
            os._exit(127)
        os.set_blocking(self.fd, False)
        self.resize(40, 120)
        self._resume_reading()
        self.input_task = asyncio.create_task(self._write_input())
        self.auth_task = asyncio.create_task(self._watch_auth())
        log.info("[Terminal] session=%s event=created pid=%d", self.id[:8], self.pid)

    @property
    def buffered(self):
        return self.end - self.acked

    def _resume_reading(self):
        if not self.reading and not self.closed and not self.eof and self.buffered < MAX_OUTPUT:
            self.loop.add_reader(self.fd, self._readable)
            self.reading = True

    def _pause_reading(self):
        if self.reading:
            self.loop.remove_reader(self.fd)
            self.reading = False

    def _poll_child(self):
        if self.reaped:
            return
        try:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.reaped = True
                self.exit_code = os.waitstatus_to_exitcode(status)
        except ChildProcessError:
            self.reaped = True

    def _readable(self):
        remaining = MAX_OUTPUT - self.buffered
        if remaining <= 0:
            self._pause_reading()
            return
        try:
            data = os.read(self.fd, min(CHUNK_SIZE, remaining))
        except BlockingIOError:
            return
        except OSError:
            data = b""
        if data:
            self.end += len(data)
            self.output.append((self.end, data))
            if self.buffered >= MAX_OUTPUT:
                self._pause_reading()
        else:
            self.eof = True
            self._pause_reading()
            self._poll_child()
        self.changed.set()

    def ack(self, offset):
        if isinstance(offset, bool) or not isinstance(offset, int) or not 0 <= offset <= self.end:
            raise ValueError("invalid output offset")
        if offset <= self.acked:
            return
        self.acked = offset
        while self.output and self.output[0][0] <= offset:
            self.output.popleft()
        if self.output:
            end, data = self.output[0]
            start = end - len(data)
            if start < offset:
                self.output[0] = (end, data[offset-start:])
        self._resume_reading()
        self.changed.set()

    def pending(self, offset):
        if offset < self.acked or offset > self.end:
            raise ValueError("output no longer available")
        # Snapshot before awaiting websocket writes: ACKs may mutate the deque.
        return [(end, data[max(0, offset-(end-len(data))):])
                for end, data in self.output if end > offset]

    async def attach(self, websocket, offset):
        if self.closed or self.closing or not self.acked <= offset <= self.end:
            raise ValueError("session state unavailable")
        old = self.socket
        self.socket = websocket
        if self.expiry:
            self.expiry.cancel()
            self.expiry = None
        self.ack(offset)
        self.changed.set()
        if old and old is not websocket:
            with suppress(Exception):
                await asyncio.wait_for(old.close(code=4001, reason="session resumed elsewhere"), .5)

    def detach(self, websocket):
        if self.socket is not websocket or self.closed or self.closing:
            return
        self.socket = None
        self.changed.set()
        async def expire():
            if self.socket is None:
                await self.close("resume-timeout")
        self.expiry = self.loop.call_later(
            RESUME_GRACE, lambda: asyncio.create_task(expire()))

    def resize(self, rows, cols):
        if not self.closed and 1 <= rows <= 4096 and 1 <= cols <= 4096:
            with suppress(OSError):
                fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))

    def feed_input(self, data):
        if self.closed or self.eof:
            return
        if len(self.input) + len(data) > MAX_INPUT:
            raise ValueError("terminal input too large")
        self.input.extend(data)
        self.input_changed.set()

    async def _write_input(self):
        # Keep receiving output ACKs even when a pasted input fills the PTY.
        while not self.closed and not self.eof:
            self.input_changed.clear()
            while self.input and not self.closed and not self.eof:
                try:
                    written = os.write(self.fd, bytes(self.input[:16384]))
                    del self.input[:written]
                    await asyncio.sleep(0)
                except BlockingIOError:
                    ready = self.loop.create_future()
                    def writable():
                        if not ready.done():
                            ready.set_result(None)
                    self.loop.add_writer(self.fd, writable)
                    try:
                        await ready
                    finally:
                        self.loop.remove_writer(self.fd)
                except OSError:
                    return
            await self.input_changed.wait()

    async def _watch_auth(self):
        while not self.closed:
            await asyncio.sleep(AUTH_CHECK_INTERVAL)
            if not get_session(self.owner):
                await self.close("authentication-expired", 1008)
                return
            if self.eof:
                self._poll_child()

    async def close(self, reason, code=1000):
        if self.closing is None:
            self.closing = asyncio.create_task(self._close(reason, code))
        await asyncio.shield(self.closing)

    async def _close(self, reason, code):
        self.closed = True
        if self.expiry:
            self.expiry.cancel()
        self._pause_reading()
        self.changed.set()
        for task in (self.input_task, self.auth_task):
            task.cancel()
        await asyncio.gather(self.input_task, self.auth_task, return_exceptions=True)
        with suppress(OSError):
            os.close(self.fd)
        self._poll_child()
        if not self.reaped:
            with suppress(ProcessLookupError):
                os.kill(self.pid, signal.SIGHUP)
            await asyncio.sleep(.1)
            self._poll_child()
        if not self.reaped:
            with suppress(ProcessLookupError):
                os.killpg(self.pid, signal.SIGKILL)
            with suppress(ProcessLookupError):
                os.kill(self.pid, signal.SIGKILL)
            with suppress(ChildProcessError):
                _, status = await asyncio.to_thread(os.waitpid, self.pid, 0)
                self.exit_code = os.waitstatus_to_exitcode(status)
                self.reaped = True
        if self.socket:
            with suppress(Exception):
                await asyncio.wait_for(self.socket.close(code=code, reason=reason), 1)
        self.on_close(self)
        log.info("[Terminal] session=%s event=closed cause=%s exit=%s age=%.1fs output=%d pending=%d",
                 self.id[:8], reason, self.exit_code, time.monotonic()-self.created,
                 self.end, self.buffered)


class TerminalSessionPool:
    def __init__(self):
        self.sessions = {}

    def acquire(self, owner, session_id, resume=False):
        session = self.sessions.get(session_id)
        if session:
            if session.owner != owner:
                raise PermissionError("terminal owner mismatch")
            if session.closed or session.closing:
                raise LookupError("terminal session ended")
            return session, True
        if resume:
            raise LookupError("terminal session expired")
        if len(self.sessions) >= MAX_TOTAL or sum(s.owner == owner for s in self.sessions.values()) >= MAX_PER_OWNER:
            raise OverflowError("terminal session limit reached")
        session = PtySession(owner, session_id, self._remove)
        self.sessions[session_id] = session
        return session, False

    def _remove(self, session):
        if self.sessions.get(session.id) is session:
            del self.sessions[session.id]

    async def shutdown(self):
        await asyncio.gather(*(s.close("service-shutdown", 1012)
                               for s in list(self.sessions.values())))


terminal_sessions = TerminalSessionPool()
