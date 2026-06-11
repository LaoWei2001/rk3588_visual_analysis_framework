import asyncio
import os
import pty
import struct
import fcntl
import termios
import signal
import shutil
import logging

from fastapi import APIRouter, WebSocket

from services.auth_service import get_session

router = APIRouter()
log = logging.getLogger("terminal")


def _set_winsize(fd: int, rows: int, cols: int) -> None:
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


@router.websocket("/ws/terminal")
async def websocket_terminal(websocket: WebSocket):
    token = websocket.query_params.get("token", "")
    if not get_session(token):
        await websocket.close(code=1008)
        return

    await websocket.accept()

    pid, fd = pty.fork()
    if pid == 0:
        # 子进程：尽量和板端 SSH 登录一致
        os.environ["TERM"] = os.environ.get("TERM", "xterm-256color")
        os.environ.setdefault("LANG", "C.UTF-8")
        try:
            os.chdir("/opt/ai_apps")
        except Exception:
            pass
        shell_path = shutil.which("bash") or "/bin/sh"
        os.execlp(shell_path, shell_path, "-l")
        os._exit(1)

    loop = asyncio.get_event_loop()

    # 先给一个合法初始尺寸，避免 vim 等程序因 0x0 卡死；前端连上后会发真实尺寸覆盖。
    _set_winsize(fd, 40, 120)
    os.set_blocking(fd, False)

    # PTY master 是字符设备，用 loop.add_reader 直接读裸 fd，再用队列按序回传 websocket。
    out_queue: "asyncio.Queue[bytes | None]" = asyncio.Queue()

    def on_readable():
        try:
            data = os.read(fd, 65536)
        except BlockingIOError:
            return
        except OSError:
            data = b""  # slave 关闭（子进程退出）会抛 EIO
        if not data:
            try:
                loop.remove_reader(fd)
            except Exception:
                pass
            out_queue.put_nowait(None)  # EOF 哨兵
            return
        out_queue.put_nowait(data)

    loop.add_reader(fd, on_readable)

    async def pty_to_ws():
        while True:
            chunk = await out_queue.get()
            if chunk is None:
                break
            try:
                await websocket.send_bytes(chunk)
            except Exception:
                break

    def safe_write(data: bytes):
        try:
            os.write(fd, data)
        except (BlockingIOError, OSError):
            pass

    async def ws_to_pty():
        while True:
            try:
                data = await websocket.receive()
            except Exception:
                break

            if data["type"] == "websocket.disconnect":
                break

            if data.get("bytes") is not None:
                safe_write(data["bytes"])
            elif data.get("text") is not None:
                text = data["text"]
                if text.startswith("\x1b[resize:"):
                    try:
                        rows_s, cols_s = text[len("\x1b[resize:"):].split("x")
                        rows, cols = int(rows_s), int(cols_s)
                        # 忽略非法/退化尺寸，否则 SIGWINCH 会把 vim 重绘进 0 宽视口
                        if rows >= 1 and cols >= 1:
                            _set_winsize(fd, rows, cols)
                            os.kill(pid, signal.SIGWINCH)
                    except Exception:
                        pass
                else:
                    safe_write(text.encode())

    pty_task = asyncio.create_task(pty_to_ws())
    input_task = asyncio.create_task(ws_to_pty())

    try:
        # 任一方向结束（子进程退出或 ws 断开）就收尾
        await asyncio.wait(
            {pty_task, input_task}, return_when=asyncio.FIRST_COMPLETED
        )
    finally:
        for t in (pty_task, input_task):
            t.cancel()
        try:
            loop.remove_reader(fd)
        except Exception:
            pass
        # 关闭 master：给 PTY 前台进程组发 SIGHUP，vim/top 等前台程序随之退出
        try:
            os.close(fd)
        except Exception:
            pass
        # 先 SIGHUP 让 bash 挂断并清理自己的作业，给一点时间，再 SIGKILL 兜底整个进程组
        try:
            os.kill(pid, signal.SIGHUP)
        except Exception:
            pass
        await asyncio.sleep(0.1)
        try:
            os.killpg(pid, signal.SIGKILL)  # pty.fork 已 setsid，pid 即进程组长
        except Exception:
            try:
                os.kill(pid, signal.SIGKILL)
            except Exception:
                pass
        try:
            os.waitpid(pid, 0)  # 回收 bash，避免僵尸；其子进程由 init 回收
        except Exception:
            pass
