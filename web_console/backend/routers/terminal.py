import asyncio
import json
import logging
import re
import secrets
import struct
import time

from fastapi import APIRouter, WebSocket
from services.auth_service import get_session
from services.terminal_sessions import terminal_sessions

router = APIRouter()
log = logging.getLogger("uvicorn.error")
HEARTBEAT_INTERVAL = 15
SESSION_ID = re.compile(r"^[a-f0-9]{32}$")
RESIZE = re.compile(r"^\x1b\[resize:(\d+)x(\d+)$")


@router.websocket("/ws/terminal")
async def websocket_terminal(websocket: WebSocket):
    token = websocket.query_params.get("token", "")
    if not get_session(token):
        await websocket.close(code=1008)
        return
    managed = websocket.query_params.get("protocol") == "2"
    session_id = websocket.query_params.get("sid", "") if managed else secrets.token_hex(16)
    try:
        offset = int(websocket.query_params.get("offset", "0")) if managed else 0
        if not SESSION_ID.fullmatch(session_id) or offset < 0:
            raise ValueError()
    except ValueError:
        await websocket.close(code=1008)
        return
    await websocket.accept()
    session, resumed = None, False
    try:
        session, resumed = terminal_sessions.acquire(
            token, session_id, managed and websocket.query_params.get("resume") == "1")
        await session.attach(websocket, offset)
    except PermissionError:
        await websocket.close(code=1008, reason="terminal owner mismatch")
        return
    except (LookupError, ValueError):
        if session is not None and not resumed:
            await session.close("invalid-initial-offset")
        await websocket.close(code=4004, reason="terminal session unavailable")
        return
    except OverflowError:
        await websocket.close(code=1013, reason="terminal session limit")
        return

    started = time.monotonic()
    close_code, cause = 1000, "transport-closed"
    send_lock = asyncio.Lock()

    async def control(kind, **fields):
        async with send_lock:
            await websocket.send_text(json.dumps({"type": f"terminal.{kind}", **fields}))

    async def send_output():
        cursor = offset
        while not session.closed and session.socket is websocket:
            session.changed.clear()
            cursor = max(cursor, session.acked)
            for end, data in session.pending(cursor):
                if session.socket is not websocket or session.closed:
                    return "replaced"
                async with send_lock:
                    await websocket.send_bytes((struct.pack("!Q", end) if managed else b"") + data)
                cursor = end
                if not managed:
                    session.ack(end)
            if session.eof and cursor == session.end and session.acked == session.end:
                if managed:
                    await control("exit", exit_code=session.exit_code)
                await session.close("shell-exited")
                return "shell-exited"
            await session.changed.wait()
        return "session-closed"

    async def receive_input():
        nonlocal close_code
        while session.socket is websocket and not session.closed:
            data = await websocket.receive()
            if session.socket is not websocket or session.closed:
                return "replaced"
            if data["type"] == "websocket.disconnect":
                close_code = data.get("code", 1006)
                return "websocket-disconnect"
            binary, text = data.get("bytes"), data.get("text")
            if binary is not None and managed:
                try:
                    if len(binary) > 4096:
                        raise ValueError()
                    message = json.loads(binary)
                    kind = message["type"]
                    if kind in ("ack", "pong"):
                        session.ack(message["offset"])
                    elif kind == "close":
                        await session.close("client-close")
                        return "client-close"
                    else:
                        raise ValueError()
                except (ValueError, KeyError, TypeError, UnicodeError):
                    close_code = 1002
                    await websocket.close(code=1002, reason="invalid terminal control")
                    return "protocol-error"
            elif text is not None:
                match = RESIZE.fullmatch(text)
                if match:
                    session.resize(*map(int, match.groups()))
                else:
                    try:
                        session.feed_input(text.encode())
                    except ValueError:
                        close_code = 1009
                        await websocket.close(code=1009, reason="terminal input limit")
                        return "input-limit"
            elif binary is not None:
                try:
                    session.feed_input(binary)
                except ValueError:
                    close_code = 1009
                    await websocket.close(code=1009, reason="terminal input limit")
                    return "input-limit"
        return "replaced"

    async def heartbeat():
        while True:
            await asyncio.sleep(HEARTBEAT_INTERVAL)
            if session.closed or session.socket is not websocket:
                return "session-closed"
            await control("ping")

    tasks = []
    try:
        if managed:
            await control("ready", resumed=resumed, offset=offset)
        log.info("[Terminal] session=%s event=%s offset=%d pending=%d",
                 session_id[:8], "resumed" if resumed else "attached", offset, session.buffered)
        tasks = [asyncio.create_task(send_output()), asyncio.create_task(receive_input())]
        if managed:
            tasks.append(asyncio.create_task(heartbeat()))
        done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            try:
                cause = task.result()
            except Exception as exc:
                cause = type(exc).__name__
                close_code = getattr(exc, "code", 1006)
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        if session.closing:
            await asyncio.shield(session.closing)
        if managed:
            session.detach(websocket)
        else:
            await session.close("legacy-disconnect")
        log.info("[Terminal] session=%s event=detached cause=%s code=%s age=%.1fs output=%d ack=%d pending=%d",
                 session_id[:8], cause, close_code, time.monotonic()-started,
                 session.end, session.acked, session.buffered)
