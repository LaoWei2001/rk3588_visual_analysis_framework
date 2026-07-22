from fastapi import APIRouter, WebSocket

from services.log_streamer import stream_log
from services.log_buffer import get_log_buffer
from services.auth_service import get_session

router = APIRouter()


@router.get("/api/apps/{name}/log")
async def get_log_tail(name: str, lines: int = 200):
    """返回最近 N 行日志（来自内存缓冲，非文件）。"""
    buf = get_log_buffer(name)
    return {"lines": buf.get_tail(lines)}


@router.websocket("/ws/logs/{name}")
async def websocket_logs(websocket: WebSocket, name: str):
    token = websocket.query_params.get("token", "")
    if not get_session(token):
        await websocket.close(code=1008)
        return
    await stream_log(websocket, name)
