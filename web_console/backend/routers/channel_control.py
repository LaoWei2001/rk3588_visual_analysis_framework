import json
import os
import socket
from pathlib import Path
from typing import Any, Dict, List
from uuid import uuid4

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from services import process_manager as pm

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

router = APIRouter()


class ChannelActionRequest(BaseModel):
    payload: Dict[str, Any] = Field(default_factory=dict)


def _app_dir(name: str) -> Path:
    app_dir = APPS_ROOT / name
    if not app_dir.exists():
        raise HTTPException(status_code=404, detail=f"App '{name}' not found")
    return app_dir


def _load_json(path: Path, default: Any) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return default


def _logic_defs(app_dir: Path) -> Dict[str, Dict[str, Any]]:
    data = _load_json(app_dir / "logics.json", {})
    items = data.get("channel_logics", [])
    out: Dict[str, Dict[str, Any]] = {}
    for item in items:
        if isinstance(item, dict) and isinstance(item.get("name"), str):
            out[item["name"]] = item
    return out


def _enabled_channels(channels: Any) -> List[tuple[int, Dict[str, Any]]]:
    """Return enabled channels keyed only by their configured channel ID."""
    enabled: List[tuple[int, Dict[str, Any]]] = []
    if not isinstance(channels, list):
        return []
    for source_index, channel in enumerate(channels):
        if not isinstance(channel, dict) or not bool(channel.get("enable", True)):
            continue
        try:
            channel_id = int(channel.get("id", source_index))
        except (TypeError, ValueError):
            continue
        enabled.append((channel_id, channel))
    enabled.sort(key=lambda item: item[0])
    return enabled


@router.get("/apps/{name}/channel-actions")
async def get_channel_actions(name: str):
    app_dir = _app_dir(name)
    config_name = "config.json"
    run_config = app_dir / "run.config"
    if run_config.exists():
        config_name = run_config.read_text(encoding="utf-8").strip() or "config.json"

    config = _load_json(app_dir / "assets" / config_name, {})
    channels = config.get("channels", []) if isinstance(config, dict) else []
    logic_defs = _logic_defs(app_dir)

    out_channels = []
    for channel_id, ch in _enabled_channels(channels):
        logic_name = str(ch.get("logic") or "").strip()
        if not logic_name:
            out_channels.append({
                "channel_id": channel_id,
                "enabled": True,
                "logic": "",
                "logic_label": "未配置后处理",
                "actions": [],
            })
            continue
        logic_def = logic_defs.get(logic_name, {})
        actions = logic_def.get("actions", [])
        if not isinstance(actions, list):
            actions = []
        out_channels.append({
            "channel_id": channel_id,
            "enabled": True,
            "logic": logic_name,
            "logic_label": str(logic_def.get("label") or logic_name),
            "actions": actions,
        })

    return {
        "socket_ready": (app_dir / "run.control.sock").exists(),
        "channels": out_channels,
    }


@router.post("/apps/{name}/channels/{channel_id}/actions/{action}")
async def post_channel_action(name: str, channel_id: int, action: str, req: ChannelActionRequest):
    app_dir = _app_dir(name)
    status = pm.get_status(name)
    if status.get("status") != "running":
        raise HTTPException(status_code=409, detail="app is not running")

    socket_path = app_dir / "run.control.sock"
    if not socket_path.exists():
        raise HTTPException(status_code=503, detail="channel control socket not ready")

    message = {
        "request_id": uuid4().hex,
        "channel_id": channel_id,
        "action": action,
        "payload": req.payload or {},
    }

    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(3.0)
            client.connect(str(socket_path))
            client.sendall((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
            raw = client.recv(64 * 1024)
    except FileNotFoundError:
        raise HTTPException(status_code=503, detail="channel control socket not found")
    except OSError as e:
        raise HTTPException(status_code=503, detail=f"channel control unavailable: {e}")

    try:
        resp = json.loads(raw.decode("utf-8").strip() or "{}")
    except Exception:
        raise HTTPException(status_code=502, detail="invalid response from channel control")

    if not resp.get("ok"):
        raise HTTPException(status_code=409, detail=resp.get("message") or "channel action rejected")
    return resp
