"""本地事件发件箱浏览、媒体查看、重试和删除 API。"""

import json
import os
import re
import shutil
import time
import secrets
import threading
import zipfile
from collections import deque
from pathlib import Path
from typing import Optional

from fastapi import APIRouter, HTTPException
from fastapi.responses import FileResponse, StreamingResponse
from pydantic import BaseModel, Field

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
CAP_BYTES = int(os.environ.get("EVENT_STORE_MAX_BYTES", 1024 * 1024 * 1024))
_SAFE_ID = re.compile(r"^[A-Za-z0-9._-]+$")
RETRY_REQUEST_FILE = "delivery_retry.request.json"

from services.data_dir import data_dir

router = APIRouter()
_exports = {}
_export_lock = threading.Lock()


class RecordSelection(BaseModel):
    ids: Optional[list[str]] = Field(default=None, max_length=5000)


def _selected_paths(name: str, ids: Optional[list[str]]) -> list[Path]:
    if ids is not None:
        if not ids or any(not _SAFE_ID.fullmatch(value) or value in (".", "..") for value in ids):
            raise HTTPException(400, "请选择有效的告警记录")
        store = _store_dir(name).resolve()
        paths = [store / value for value in dict.fromkeys(ids)]
        if any(path.is_symlink() or path.resolve().parent != store or
               (path.is_dir() and not (path / "event.json").is_file()) for path in paths):
            raise HTTPException(400, "选择包含无效的告警记录")
        return paths
    store = _store_dir(name)
    return sorted((path for path in store.iterdir() if not path.is_symlink() and path.is_dir() and (path / "event.json").is_file()),
                  key=lambda path: path.name) if store.exists() else []


@router.post("/apps/{name}/records/delete-selected")
async def delete_selected_records(name: str, selection: RecordSelection):
    if selection.ids is None:
        raise HTTPException(400, "请选择要清空的记录")
    deleted, failed = [], []
    for path in _selected_paths(name, selection.ids):
        if not path.is_dir():
            continue
        try:
            shutil.rmtree(path)
            deleted.append(path.name)
        except OSError:
            failed.append(path.name)
    return {"ok": not failed, "deleted_ids": deleted, "failed_ids": failed}


@router.post("/apps/{name}/records/export-images")
async def prepare_image_export(name: str, selection: RecordSelection):
    paths = _selected_paths(name, selection.ids)
    images = [{"id": path.name, "filename": f"{path.name}_raw.jpg"}
              for path in paths if (path / "raw.jpg").is_file()]
    if not images:
        raise HTTPException(404, "所选范围内没有已生成的原始图片")
    filename = f"alarm_raw_images_{time.strftime('%Y%m%d_%H%M%S')}.zip"
    download_id = secrets.token_urlsafe(24)
    with _export_lock:
        now = time.monotonic()
        for key in list(_exports):
            if _exports[key][0] < now:
                del _exports[key]
        if len(_exports) >= 50:
            del _exports[next(iter(_exports))]
        _exports[download_id] = (now + 1800, name, images, filename)
    return {"download_id": download_id, "filename": filename, "images": images,
            "image_count": len(images), "skipped_count": len(paths) - len(images)}


class _ZipOutput:
    """不可 seek 的 ZIP 输出；每次只保留正在传输的一小块，不写板端临时文件。"""
    def __init__(self):
        self.chunks = deque()
        self.position = 0

    def write(self, chunk):
        if chunk:
            self.chunks.append(bytes(chunk))
            self.position += len(chunk)
        return len(chunk)

    def tell(self):
        return self.position

    def flush(self):
        pass


def _stream_raw_images(name: str, images: list[dict]):
    output = _ZipOutput()
    missing = []
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as archive:
        for image in images:
            path = _store_dir(name) / image["id"] / "raw.jpg"
            try:
                source = path.open("rb")
            except OSError:
                missing.append(image["id"])
                continue
            with source, archive.open(image["filename"], "w", force_zip64=True) as destination:
                while True:
                    chunk = source.read(65536)
                    if not chunk:
                        break
                    destination.write(chunk)
                    while output.chunks:
                        yield output.chunks.popleft()
            while output.chunks:
                yield output.chunks.popleft()
        if missing:
            archive.writestr("missing-images.txt", "图片在导出期间已被删除：\n" + "\n".join(missing))
    while output.chunks:
        yield output.chunks.popleft()


@router.get("/apps/{name}/records/export-images/{download_id}")
async def download_image_export(name: str, download_id: str):
    with _export_lock:
        job = _exports.get(download_id)
    if not job or job[0] < time.monotonic() or job[1] != name:
        raise HTTPException(404, "导出已过期，请重新导出")
    return StreamingResponse(_stream_raw_images(name, job[2]), media_type="application/zip",
                             headers={"Content-Disposition": f'attachment; filename="{job[3]}"',
                                      "Cache-Control": "no-store", "X-Accel-Buffering": "no"})


@router.get("/apps/{name}/records/{event_id}/raw-image")
async def record_original_image(name: str, event_id: str):
    image = _event_dir(name, event_id) / "raw.jpg"
    if not image.is_file():
        raise HTTPException(404, "原始图片不存在或仍在生成")
    return FileResponse(image, media_type="image/jpeg", headers={"Cache-Control": "no-store"})


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
    if not _SAFE_ID.fullmatch(event_id) or event_id in (".", ".."):
        raise HTTPException(400, "非法的事件 ID")
    path = _store_dir(name) / event_id
    if path.is_symlink() or not path.is_dir():
        raise HTTPException(404, "事件不存在或已成功投递")
    return path


@router.get("/apps/{name}/records")
async def list_records(
    name: str,
    limit: int = 500,
    start_unix_ms: Optional[int] = None,
    end_unix_ms: Optional[int] = None,
):
    if start_unix_ms is not None and start_unix_ms < 0:
        raise HTTPException(400, "开始时间无效")
    if end_unix_ms is not None and end_unix_ms < 0:
        raise HTTPException(400, "结束时间无效")
    if start_unix_ms is not None and end_unix_ms is not None and start_unix_ms > end_unix_ms:
        raise HTTPException(400, "开始时间不能晚于结束时间")

    store = _store_dir(name)
    if not store.exists():
        return {
            "records": [], "count": 0, "filtered_count": 0,
            "total_bytes": 0, "cap_bytes": CAP_BYTES,
        }
    records = []
    record_count = 0
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
        record_count += 1
        total += size
        event, source, media = meta["event"], meta["source"], meta["media"]
        trigger_unix_ms = event.get("trigger_unix_ms")
        if not isinstance(trigger_unix_ms, (int, float)) or trigger_unix_ms <= 0:
            created_unix_sec = event.get("created_unix_sec", 0)
            trigger_unix_ms = created_unix_sec * 1000 if isinstance(created_unix_sec, (int, float)) else 0
        if start_unix_ms is not None and trigger_unix_ms < start_unix_ms:
            continue
        if end_unix_ms is not None and trigger_unix_ms > end_unix_ms:
            continue
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
        "count": record_count,
        "filtered_count": len(records),
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
