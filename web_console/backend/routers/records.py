"""本地事件发件箱浏览、媒体查看、重试和删除 API。"""

import json
import os
import re
import shutil
import time
from pathlib import Path

from fastapi import APIRouter, HTTPException
from fastapi.responses import FileResponse

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
CAP_BYTES = int(os.environ.get("EVENT_STORE_MAX_BYTES", 1024 * 1024 * 1024))
_SAFE_ID = re.compile(r"^[A-Za-z0-9._-]+$")
RETRY_REQUEST_FILE = "delivery_retry.request.json"

from services.data_dir import data_dir

router = APIRouter()


def _store_dir(name: str) -> Path:
    override = os.environ.get("EVENT_STORE_DIR")
    if override:
        return Path(override)
    return data_dir(name) / "event_store"


def _read_event_dir(event_dir: Path) -> dict:
    event_doc = json.loads((event_dir / "event.json").read_text(encoding="utf-8"))
    media_doc = json.loads((event_dir / "media_state.json").read_text(encoding="utf-8"))
    delivery_doc = json.loads((event_dir / "delivery_state.json").read_text(encoding="utf-8"))
    event = event_doc.get("event", {})
    source = event_doc.get("source", {})
    data = event_doc.get("data", {})
    media_entries = media_doc.get("media", {})
    deliveries = delivery_doc.get("deliveries", [])
    if not all(isinstance(item, dict) for item in (
        event, source, data, media_doc, media_entries, delivery_doc,
    )) or not isinstance(deliveries, list):
        raise ValueError("invalid event document")
    media = {}
    media_statuses = {}
    for kind, entry in media_entries.items():
        if not isinstance(entry, dict) or not isinstance(entry.get("files", {}), dict):
            raise ValueError(f"invalid {kind} media state")
        media.update(entry["files"])
        media_statuses[str(kind)] = {
            "status": str(entry.get("status", "")),
            "error": str(entry.get("error", "")),
        }
    return {
        "schema_version": event_doc.get("schema_version", 3),
        "event": event,
        "source": source,
        "fields": data.get("fields", {}),
        "media": media,
        "media_statuses": media_statuses,
        "state": media_doc.get("status", "ready"),
        "deliveries": deliveries,
    }


def _event_dir(name: str, event_id: str) -> Path:
    if not _SAFE_ID.fullmatch(event_id):
        raise HTTPException(400, "非法的事件 ID")
    path = _store_dir(name) / event_id
    if not path.is_dir():
        raise HTTPException(404, "事件不存在或已成功投递")
    return path


@router.get("/apps/{name}/records")
async def list_records(name: str, limit: int = 500):
    store = _store_dir(name)
    if not store.exists():
        return {"records": [], "count": 0, "total_bytes": 0, "cap_bytes": CAP_BYTES}
    records = []
    total = 0
    try:
        entries = list(store.iterdir())
    except OSError:
        entries = []
    for path in entries:
        if not path.is_dir() or not (path / "event.json").is_file():
            continue
        try:
            meta = _read_event_dir(path)
            size = sum(item.stat().st_size for item in path.iterdir() if item.is_file())
        except (OSError, ValueError, TypeError):
            continue
        total += size
        event, source, media = meta["event"], meta["source"], meta["media"]
        required_media = {
            str(kind)
            for delivery in meta["deliveries"] if isinstance(delivery, dict)
            for kind in delivery.get("media", []) if isinstance(delivery.get("media", []), list)
        }
        records.append({
            "id": event.get("id", path.name),
            "channel_id": source.get("channel_id"),
            "event_type": event.get("type", ""),
            "message": event.get("message", ""),
            "trigger_count": event.get("trigger_count", 1),
            "snap_time": event.get("snap_time") or event.get("trigger_unix_ms", 0),
            "created_unix_sec": event.get("created_unix_sec", 0),
            "state": meta["state"],
            "required_media": sorted(required_media),
            "has_annotated_image": bool(media.get("annotated_image")),
            "has_raw_image": bool(media.get("raw_image")),
            "has_video": bool(media.get("video")),
            "media_statuses": meta["media_statuses"],
            "deliveries": meta["deliveries"],
            "total_bytes": size,
        })
    records.sort(key=lambda item: item.get("created_unix_sec") or 0, reverse=True)
    return {
        "records": records[:max(0, limit)],
        "count": len(records),
        "total_bytes": total,
        "cap_bytes": CAP_BYTES,
    }


@router.get("/apps/{name}/records/{event_id}/json")
async def record_json(name: str, event_id: str):
    path = _event_dir(name, event_id)
    try:
        meta = _read_event_dir(path)
    except (OSError, ValueError, TypeError) as exc:
        raise HTTPException(500, "事件 JSON 读取失败") from exc
    return {
        "schema_version": meta["schema_version"],
        "event": meta["event"],
        "source": meta["source"],
        "fields": meta["fields"],
        "media": meta["media"],
        "media_statuses": meta["media_statuses"],
        "deliveries": meta["deliveries"],
    }


@router.get("/apps/{name}/records/{event_id}/image")
async def record_image(name: str, event_id: str, raw: int = 0):
    path = _event_dir(name, event_id)
    image = path / ("raw.jpg" if raw else "annotated.jpg")
    if not image.is_file() and raw:
        image = path / "annotated.jpg"
    if not image.is_file():
        raise HTTPException(404, "图片不存在或仍在生成")
    return FileResponse(str(image), media_type="image/jpeg", headers={"Cache-Control": "no-cache"})


@router.get("/apps/{name}/records/{event_id}/video")
async def record_video(name: str, event_id: str):
    video = _event_dir(name, event_id) / "clip.mp4"
    if not video.is_file():
        raise HTTPException(404, "视频不存在或仍在生成")
    return FileResponse(
        str(video), media_type="video/mp4",
        headers={"Cache-Control": "no-cache", "Accept-Ranges": "bytes",
                 "Content-Disposition": "inline"},
    )


@router.post("/apps/{name}/records/{event_id}/retry")
async def retry_record(name: str, event_id: str):
    path = _event_dir(name, event_id)
    try:
        _read_event_dir(path)
    except (OSError, ValueError, TypeError) as exc:
        raise HTTPException(500, "事件状态读取失败") from exc
    request = path / RETRY_REQUEST_FILE
    temporary = request.with_name(request.name + ".web.tmp")
    temporary.write_text(
        json.dumps({"requested_unix_ms": int(time.time() * 1000)}, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, request)
    return {"ok": True, "accepted": True}


@router.delete("/apps/{name}/records/{event_id}")
async def delete_record(name: str, event_id: str):
    path = _event_dir(name, event_id)
    shutil.rmtree(path)
    return {"ok": True}


@router.delete("/apps/{name}/records")
async def delete_all_records(name: str):
    store = _store_dir(name)
    deleted = 0
    if store.exists():
        for entry in list(store.iterdir()):
            if entry.is_dir() and (entry / "event.json").is_file():
                try:
                    shutil.rmtree(entry)
                    deleted += 1
                except OSError:
                    pass
    return {"ok": True, "deleted": deleted}
