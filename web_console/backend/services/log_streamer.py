"""
log_streamer.py — WebSocket 日志推送

从内存缓冲（log_buffer.AppLogBuffer）读取日志，不依赖任何磁盘文件。
"""

import asyncio
from fastapi import WebSocket, WebSocketDisconnect

from services.log_buffer import get_log_buffer


async def stream_log(websocket: WebSocket, app_name: str) -> None:
    await websocket.accept()

    buf = get_log_buffer(app_name)
    q: asyncio.Queue = asyncio.Queue()
    buf.subscribe(q)

    try:
        # 先把缓冲中已有的行一次性发送给客户端
        tail = buf.get_tail(300)
        if tail:
            await websocket.send_text("\n".join(tail) + "\n")

        # 持续接收新行
        while True:
            try:
                line = await asyncio.wait_for(q.get(), timeout=20.0)
                await websocket.send_text(line + "\n")
                if line == "[进程已停止]":
                    break
            except asyncio.TimeoutError:
                # 发送空帧作为心跳，检测断线
                try:
                    await websocket.send_text("")
                except Exception:
                    break
    except WebSocketDisconnect:
        pass
    except Exception as e:
        try:
            await websocket.send_text(f"\n[日志流异常: {e}]\n")
        except Exception:
            pass
    finally:
        buf.unsubscribe(q)
