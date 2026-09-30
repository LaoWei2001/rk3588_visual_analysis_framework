"""Low-coupling control API for timelines of local-file video sources."""

import asyncio
import json
import os
import socket
import subprocess
from pathlib import Path
from typing import Any, Dict

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from services import process_manager as pm

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
router = APIRouter()
_DURATION_CACHE: Dict[str, tuple[int, int, int]] = {}


class SeekRequest(BaseModel):
    position_ms: int = Field(ge=0)


def _app_dir(name: str) -> Path:
    app_dir = APPS_ROOT / name
    if not app_dir.exists():
        raise HTTPException(status_code=404, detail=f"App '{name}' not found")
    return app_dir


def _socket_request(socket_path: Path, message: Dict[str, Any]) -> Dict[str, Any]:
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(3.0)
            client.connect(str(socket_path))
            client.sendall((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
            chunks = []
            while True:
                chunk = client.recv(64 * 1024)
                if not chunk:
                    break
                chunks.append(chunk)
                if b"\n" in chunk:
                    break
    except FileNotFoundError as exc:
        raise HTTPException(status_code=503, detail="文件播放控制通道尚未就绪") from exc
    except OSError as exc:
        raise HTTPException(status_code=503, detail=f"文件播放控制不可用：{exc}") from exc

    try:
        response = json.loads(b"".join(chunks).decode("utf-8").strip() or "{}")
    except Exception as exc:
        raise HTTPException(status_code=502, detail="文件播放控制返回了无效数据") from exc
    if not isinstance(response, dict):
        raise HTTPException(status_code=502, detail="文件播放控制返回格式错误")
    return response


def _probe_duration_ms(app_dir: Path, location: str) -> int:
    """Use ffprobe only when GStreamer cannot report duration; cache by file stat."""
    if not location:
        return 0
    path = Path(location)
    if not path.is_absolute():
        path = app_dir / path
    try:
        path = path.resolve(strict=True)
        stat = path.stat()
    except OSError:
        return 0
    key = str(path)
    cached = _DURATION_CACHE.get(key)
    if cached and cached[0] == stat.st_mtime_ns and cached[1] == stat.st_size:
        return cached[2]
    try:
        result = subprocess.run(
            [
                "ffprobe", "-v", "error", "-show_entries", "format=duration",
                "-of", "default=noprint_wrappers=1:nokey=1", str(path),
            ],
            check=True,
            capture_output=True,
            text=True,
            timeout=8,
        )
        duration_ms = max(0, int(round(float(result.stdout.strip()) * 1000.0)))
    except (OSError, ValueError, subprocess.SubprocessError):
        duration_ms = 0
    _DURATION_CACHE[key] = (stat.st_mtime_ns, stat.st_size, duration_ms)
    return duration_ms


def _fill_missing_durations(app_dir: Path, sources: Any) -> list:
    if not isinstance(sources, list):
        return []
    enriched = []
    for source in sources:
        if not isinstance(source, dict):
            continue
        item = dict(source)
        try:
            duration_ms = int(item.get("duration_ms") or 0)
        except (TypeError, ValueError):
            duration_ms = 0
        if duration_ms <= 0:
            duration_ms = _probe_duration_ms(app_dir, str(item.get("location") or ""))
            item["duration_ms"] = duration_ms
        enriched.append(item)
    return enriched


@router.get("/apps/{name}/file-playback")
async def get_file_playback(name: str):
    app_dir = _app_dir(name)
    if pm.get_status(name).get("status") != "running":
        return {"socket_ready": False, "sources": []}
    socket_path = app_dir / "run.playback.sock"
    if not socket_path.exists():
        return {"socket_ready": False, "sources": []}
    response = await asyncio.to_thread(_socket_request, socket_path, {"command": "status"})
    if not response.get("ok"):
        raise HTTPException(status_code=502, detail=response.get("message") or "读取视频进度失败")
    sources = await asyncio.to_thread(_fill_missing_durations, app_dir, response.get("sources", []))
    return {"socket_ready": True, "sources": sources}


@router.post("/apps/{name}/file-playback/{channel_id}/seek")
async def seek_file_playback(name: str, channel_id: int, request: SeekRequest):
    app_dir = _app_dir(name)
    if pm.get_status(name).get("status") != "running":
        raise HTTPException(status_code=409, detail="程序未运行")
    socket_path = app_dir / "run.playback.sock"
    if not socket_path.exists():
        raise HTTPException(status_code=503, detail="文件播放控制通道尚未就绪")
    response = await asyncio.to_thread(
        _socket_request,
        socket_path,
        {"command": "seek", "channel_id": channel_id, "position_ms": request.position_ms},
    )
    if not response.get("ok"):
        raise HTTPException(status_code=409, detail=response.get("message") or "视频跳转失败")
    return response
