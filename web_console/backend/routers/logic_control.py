"""Expose channel and global-logic actions through one control API."""

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


class LogicActionRequest(BaseModel):
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


def _logic_defs(app_dir: Path, catalog_key: str) -> Dict[str, Dict[str, Any]]:
    data = _load_json(app_dir / "logics.json", {})
    items = data.get(catalog_key, [])
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


@router.get("/apps/{name}/logic-actions")
async def get_logic_actions(name: str):
    app_dir = _app_dir(name)
    config_name = "config.json"
    run_config = app_dir / "run.config"
    if run_config.exists():
        config_name = run_config.read_text(encoding="utf-8").strip() or "config.json"

    config = _load_json(app_dir / "assets" / config_name, {})
    channels = config.get("channels", []) if isinstance(config, dict) else []
    channel_logic_defs = _logic_defs(app_dir, "channel_logics")
    global_logic_defs = _logic_defs(app_dir, "global_logics")

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
        logic_def = channel_logic_defs.get(logic_name, {})
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

    global_config = config.get("global", {}) if isinstance(config, dict) else {}
    global_instances = global_config.get("global_logics", []) if isinstance(global_config, dict) else []
    out_globals = []
    if isinstance(global_instances, list):
        for item in global_instances:
            if not isinstance(item, dict) or not bool(item.get("enable", True)):
                continue
            instance_id = str(item.get("instance_id") or "").strip()
            logic_name = str(item.get("logic") or "").strip()
            if not instance_id or not logic_name:
                continue
            logic_def = global_logic_defs.get(logic_name, {})
            actions = logic_def.get("actions", [])
            if not isinstance(actions, list):
                actions = []
            out_globals.append({
                "instance_id": instance_id,
                "enabled": True,
                "logic": logic_name,
                "logic_label": str(logic_def.get("label") or logic_name),
                "actions": actions,
            })

    return {
        "socket_ready": (app_dir / "run.control.sock").exists(),
        "channels": out_channels,
        "globals": out_globals,
    }


@router.post("/apps/{name}/channels/{channel_id}/actions/{action}")
async def post_channel_action(name: str, channel_id: int, action: str, req: LogicActionRequest):
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


@router.post("/apps/{name}/global-logics/{instance_id}/actions/{action}")
async def post_global_action(name: str, instance_id: str, action: str, req: LogicActionRequest):
    app_dir = _app_dir(name)
    status = pm.get_status(name)
    if status.get("status") != "running":
        raise HTTPException(status_code=409, detail="app is not running")

    socket_path = app_dir / "run.control.sock"
    if not socket_path.exists():
        raise HTTPException(status_code=503, detail="logic control socket not ready")

    message = {
        "request_id": uuid4().hex,
        "scope": "global",
        "instance_id": instance_id,
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
        raise HTTPException(status_code=503, detail="logic control socket not found")
    except OSError as exc:
        raise HTTPException(status_code=503, detail=f"logic control unavailable: {exc}")

    try:
        resp = json.loads(raw.decode("utf-8").strip() or "{}")
    except Exception:
        raise HTTPException(status_code=502, detail="invalid response from logic control")
    if not resp.get("ok"):
        raise HTTPException(status_code=409, detail=resp.get("message") or "global action rejected")
    return resp
