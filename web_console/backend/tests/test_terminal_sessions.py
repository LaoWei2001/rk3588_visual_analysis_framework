"""Real PTYs: reconnect identity, bounded output, lifecycle and auth isolation."""
import asyncio
import json
import os
import re
import struct
import time

import pytest

from routers import terminal
from services import auth_service as auth
from services import terminal_sessions as service


class Socket:
    def __init__(self, token, sid, offset=0, resume=False, managed=True):
        self.query_params = dict(token=token)
        if managed:
            self.query_params.update(protocol="2", sid=sid, offset=str(offset), resume="1" if resume else "0")
        self.incoming = asyncio.Queue()
        self.outgoing = asyncio.Queue()
        self.accepted = False
        self.code = None

    async def accept(self):
        self.accepted = True

    async def receive(self):
        return await self.incoming.get()

    async def send_text(self, value):
        await self.outgoing.put(json.loads(value))

    async def send_bytes(self, value):
        await self.outgoing.put(value)

    async def close(self, code=1000, reason=""):
        self.code = code
        await self.incoming.put(dict(type="websocket.disconnect", code=code))

    def text(self, value):
        self.incoming.put_nowait(dict(type="websocket.receive", text=value))

    def control(self, kind, offset=0):
        self.incoming.put_nowait(dict(type="websocket.receive",
                                      bytes=json.dumps(dict(type=kind, offset=offset)).encode()))


async def ready(socket):
    message = await asyncio.wait_for(socket.outgoing.get(), 3)
    assert message["type"] == "terminal.ready"
    return message


async def until(socket, pattern, offset=0, ack=True):
    data = b""
    while not re.search(pattern, data):
        message = await asyncio.wait_for(socket.outgoing.get(), 5)
        if isinstance(message, dict):
            assert message["type"] == "terminal.ping", message
            socket.control("pong", offset)
            continue
        end = struct.unpack("!Q", message[:8])[0]
        start = end-len(message[8:])
        assert start <= offset <= end
        data += message[8+offset-start:]
        offset = end
        if ack:
            socket.control("ack", offset)
    return data, offset


def run(pool, scenario):
    async def guarded():
        try:
            await scenario()
        finally:
            await pool.shutdown()
    asyncio.run(guarded())


@pytest.fixture
def setup_terminal(monkeypatch, tmp_path):
    monkeypatch.setenv("HISTFILE", "/dev/null")
    monkeypatch.setattr(service, "APPS_ROOT", tmp_path)
    pool = service.TerminalSessionPool()
    monkeypatch.setattr(terminal, "terminal_sessions", pool)
    token = auth.create_session("terminal-test")
    yield pool, token
    auth.revoke_session(token)


def test_transport_reconnect_keeps_pid_variables_and_pending_output(setup_terminal, caplog):
    pool, token = setup_terminal
    async def scenario():
        first = Socket(token, "a"*32)
        task = asyncio.create_task(terminal.websocket_terminal(first))
        assert not (await ready(first))["resumed"]
        first.text("export TERM_PROBE=retained; printf '\\nFIRST:%s:%s\\n' \"$$\" \"$TERM_PROBE\"\n")
        data, offset = await until(first, rb"\r\nFIRST:(\d+):retained\r\n")
        pid = int(re.search(rb"FIRST:(\d+):retained", data)[1])
        first.text("sleep .15; printf '\\nDELAYED:%s\\n' \"$TERM_PROBE\"\n")
        await first.close(1006)
        await asyncio.wait_for(task, 3)
        session = pool.sessions["a"*32]
        assert session.pid == pid and not session.closed
        await asyncio.sleep(.2)
        second = Socket(token, "a"*32, offset, True)
        task = asyncio.create_task(terminal.websocket_terminal(second))
        assert (await ready(second))["resumed"]
        _, offset = await until(second, rb"\r\nDELAYED:retained\r\n", offset)
        second.text("printf '\\nSECOND:%s:%s\\n' \"$$\" \"$TERM_PROBE\"\n")
        data, offset = await until(second, rb"\r\nSECOND:(\d+):retained\r\n", offset)
        assert int(re.search(rb"SECOND:(\d+):retained", data)[1]) == pid
        second.control("close", offset)
        await asyncio.wait_for(task, 3)
        assert not pool.sessions and session.reaped
        with pytest.raises(ChildProcessError):
            os.waitpid(pid, os.WNOHANG)
    with caplog.at_level("INFO", logger="uvicorn.error"):
        run(pool, scenario)
    assert "event=resumed" in caplog.text and "code=1006" in caplog.text
    assert token not in caplog.text


def test_slow_client_is_backpressured_and_receives_complete_output(setup_terminal, monkeypatch):
    pool, token = setup_terminal
    monkeypatch.setattr(service, "MAX_OUTPUT", 8192)
    async def scenario():
        socket = Socket(token, "b"*32)
        task = asyncio.create_task(terminal.websocket_terminal(socket))
        await ready(socket)
        socket.text("stty -echo; printf '\\nBEGIN\\n'; python3 -c 'import os;os.write(1,b\"X\"*200000)'; printf '\\nEND\\n'\n")
        session = pool.sessions["b"*32]
        for _ in range(200):
            if session.buffered == service.MAX_OUTPUT:
                break
            await asyncio.sleep(.01)
        assert session.buffered == 8192 and not session.reading
        data, offset = await until(socket, rb"\r\nEND\r\n")
        output = data.split(b"\r\nBEGIN\r\n", 1)[1].split(b"\r\nEND\r\n", 1)[0]
        assert output == b"X"*200000
        socket.control("close", offset)
        await asyncio.wait_for(task, 3)
        assert not pool.sessions
    run(pool, scenario)


def test_detached_session_expires_and_cannot_be_silently_recreated(setup_terminal, monkeypatch):
    pool, token = setup_terminal
    monkeypatch.setattr(service, "RESUME_GRACE", .03)
    async def scenario():
        socket = Socket(token, "c"*32)
        task = asyncio.create_task(terminal.websocket_terminal(socket))
        await ready(socket)
        session = pool.sessions["c"*32]
        await socket.close(1006)
        await asyncio.wait_for(task, 3)
        await asyncio.sleep(.2)
        await session.close("test-cleanup")
        assert session.reaped and not pool.sessions
        expired = Socket(token, "c"*32, resume=True)
        await terminal.websocket_terminal(expired)
        assert expired.code == 4004 and not pool.sessions
    run(pool, scenario)


def test_resume_is_bound_to_the_original_login(setup_terminal):
    pool, token = setup_terminal
    other = auth.create_session("another-login")
    async def scenario():
        socket = Socket(token, "d"*32)
        task = asyncio.create_task(terminal.websocket_terminal(socket))
        await ready(socket)
        wrong = Socket(other, "d"*32, resume=True)
        await terminal.websocket_terminal(wrong)
        assert wrong.code == 1008 and pool.sessions["d"*32].socket is socket
        socket.control("close")
        await asyncio.wait_for(task, 3)
    try:
        run(pool, scenario)
    finally:
        auth.revoke_session(other)


def test_revocation_closes_a_connected_pty(setup_terminal, monkeypatch):
    pool, token = setup_terminal
    monkeypatch.setattr(service, "AUTH_CHECK_INTERVAL", .02)
    async def scenario():
        socket = Socket(token, "e"*32)
        task = asyncio.create_task(terminal.websocket_terminal(socket))
        await ready(socket)
        session = pool.sessions["e"*32]
        auth.revoke_session(token)
        await asyncio.wait_for(task, 3)
        await session.close("test-cleanup")
        assert socket.code == 1008 and session.reaped and not pool.sessions
    run(pool, scenario)


def test_takeover_does_not_allow_old_socket_to_close_new_session(setup_terminal):
    pool, token = setup_terminal
    async def scenario():
        first = Socket(token, "1"*32)
        old_task = asyncio.create_task(terminal.websocket_terminal(first))
        await ready(first)
        second = Socket(token, "1"*32, resume=True)
        new_task = asyncio.create_task(terminal.websocket_terminal(second))
        await ready(second)
        first.control("close")
        await asyncio.wait_for(old_task, 3)
        session = pool.sessions["1"*32]
        assert not session.closed and session.socket is second and first.code == 4001
        second.text("printf '\\nSTILL_ALIVE\\n'\n")
        await until(second, rb"\r\nSTILL_ALIVE\r\n")
        second.control("close")
        await asyncio.wait_for(new_task, 3)
    run(pool, scenario)


def test_invalid_initial_offset_does_not_leak_a_shell(setup_terminal):
    pool, token = setup_terminal
    async def scenario():
        socket = Socket(token, "2"*32, offset=10**9)
        await terminal.websocket_terminal(socket)
        assert socket.code == 4004 and not pool.sessions
    run(pool, scenario)


def test_heartbeat_is_control_traffic_and_shell_exit_is_distinct(setup_terminal, monkeypatch):
    pool, token = setup_terminal
    monkeypatch.setattr(terminal, "HEARTBEAT_INTERVAL", .03)
    async def scenario():
        socket = Socket(token, "f"*32)
        task = asyncio.create_task(terminal.websocket_terminal(socket))
        await ready(socket)
        socket.text("printf '\\nREADY\\n'\n")
        _, offset = await until(socket, rb"\r\nREADY\r\n")
        while True:
            message = await asyncio.wait_for(socket.outgoing.get(), 3)
            if isinstance(message, dict):
                assert message["type"] == "terminal.ping"
                socket.control("pong", offset)
                break
            offset = struct.unpack("!Q", message[:8])[0]
            socket.control("ack", offset)
        socket.text("exit\n")
        while True:
            message = await asyncio.wait_for(socket.outgoing.get(), 3)
            if isinstance(message, dict) and message["type"] == "terminal.exit":
                break
            if isinstance(message, bytes):
                offset = struct.unpack("!Q", message[:8])[0]
                assert b'"type": "pong"' not in message[8:]
            socket.control("ack", offset)
        await asyncio.wait_for(task, 3)
        assert socket.code == 1000 and not pool.sessions
    run(pool, scenario)
