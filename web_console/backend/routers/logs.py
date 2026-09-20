from fastapi import APIRouter, WebSocket

from services.log_streamer import stream_log
from services import process_manager as pm
from services.auth_service import get_session

router = APIRouter()


@router.get("/api/apps/{name}/log")
async def get_log_tail(name: str, lines: int = 200):
    """返回最近 N 行日志（CLI 与 Web 共用 systemd journal）。"""
    pm.ensure_log_reader(name)
    return {"lines": pm.get_logs(name, lines)}


@router.websocket("/ws/logs/{name}")
async def websocket_logs(websocket: WebSocket, name: str):
    token = websocket.query_params.get("token", "")
    if not get_session(token):
        await websocket.close(code=1008)
        return
    pm.ensure_log_reader(name)
    await stream_log(websocket, name)
