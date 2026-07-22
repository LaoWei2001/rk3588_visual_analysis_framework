"""
GET  /api/apps/{name}/records              列出本地发件箱里的待上报记录
GET  /api/apps/{name}/records/{rid}/json   查看实际发送给 Dify 的纯业务 event_json
GET  /api/apps/{name}/records/{rid}/image  取带媒体记录的截图(带框图; ?raw=1 取原图)

数据由 C++ 产生业务事件时落盘到 <app>/alarm_store/ (env ALARM_STORE_DIR 可覆盖),
Python 上报微服务补传成功后会删除 —— 所以这里只会看到"还没传上去"的那些。
"""
import json
import os
import re
from pathlib import Path

from fastapi import APIRouter, HTTPException
from fastapi.responses import FileResponse

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
CAP_BYTES = int(os.environ.get("ALARM_STORE_MAX_BYTES", 1024 * 1024 * 1024))
_SAFE_ID = re.compile(r"^[A-Za-z0-9._-]+$")
_MISSING = object()

router = APIRouter()


def _store_dir(name: str) -> Path:
    """该 App 的发件箱目录。env ALARM_STORE_DIR 为全局覆盖, 否则 <app>/alarm_store。"""
    override = os.environ.get("ALARM_STORE_DIR")
    if override:
        return Path(override)
    return APPS_ROOT / name / "alarm_store"


def _business_event(meta: dict) -> dict:
    """与上传服务保持一致：只生成 Dify event_json，不泄露发件箱内部状态。"""
    fields = meta.get("fields", {})
    source = fields.get("event_payload", {}) if isinstance(fields, dict) else {}
    payload = dict(source) if isinstance(source, dict) else {}
    business = {
        "schema_version": 2,
        "event_id": meta.get("event_id", ""),
        "channel_id": meta.get("channel_id"),
        "trigger_unix_ms": meta.get("trigger_unix_ms"),
        "snap_time": meta.get("snap_time", ""),
        "end_time": meta.get("end_time") or meta.get("snap_time", ""),
    }
    business.update(payload)
    return business


def _lookup_business(root: dict, path: str):
    current = root
    for part in (part for part in path.split(".") if part):
        if not isinstance(current, dict) or part not in current:
            return _MISSING
        current = current[part]
    return current


def _set_path(root: dict, path: str, value):
    parts = [part for part in path.split(".") if part]
    if not parts:
        raise ValueError("mapped JSON key is empty")
    current = root
    for part in parts[:-1]:
        child = current.get(part)
        if not isinstance(child, dict):
            child = {}
            current[part] = child
        current = child
    current[parts[-1]] = value


def _coerce(value, value_type: str):
    if value is None or not value_type:
        return value
    if value_type == "string":
        return str(value)
    if value_type == "number":
        number = float(value)
        return int(number) if number.is_integer() else number
    if value_type == "boolean":
        if isinstance(value, bool):
            return value
        text = str(value).strip().lower()
        if text in ("true", "1", "yes", "on"):
            return True
        if text in ("false", "0", "no", "off"):
            return False
        raise ValueError(f"invalid boolean value: {value}")
    if value_type == "json":
        return json.loads(value) if isinstance(value, str) else value
    raise ValueError(f"unsupported input type: {value_type}")


def _mapped_business_event(meta: dict, delivery: dict) -> dict:
    """与上传微服务一致：缺少 event_fields 的旧记录全量返回，显式映射则按路径组装。"""
    complete = _business_event(meta)
    if "event_fields" not in delivery:
        return complete

    mappings = delivery.get("event_fields")
    if not isinstance(mappings, list):
        raise ValueError("delivery event_fields must be a list")
    result = {}
    for mapping in mappings:
        if not isinstance(mapping, dict):
            raise ValueError("event field mapping must be an object")
        source = str(mapping.get("source", "")).strip()
        target = str(mapping.get("target", "")).strip()
        required = bool(mapping.get("required", False))
        if not source or not target:
            if required:
                raise ValueError("required event field source/target is empty")
            continue
        value = _lookup_business(complete, source)
        if value is _MISSING:
            if required:
                raise ValueError(f"missing required event field: {source}")
            continue
        _set_path(result, target, _coerce(value, str(mapping.get("type", ""))))
    return result


def _dify_payloads(meta: dict) -> list:
    payloads = []
    deliveries = meta.get("deliveries", [])
    if not isinstance(deliveries, list):
        return payloads
    for delivery in deliveries:
        if not isinstance(delivery, dict) or delivery.get("target") != "dify":
            continue
        payloads.append({
            "delivery_id": str(delivery.get("id", "")),
            "media": str(delivery.get("media", "")),
            "status": str(delivery.get("status", "")),
            "event_variable": str(delivery.get("event_variable", "")).strip(),
            "event_json": _mapped_business_event(meta, delivery),
        })
    return payloads


@router.get("/apps/{name}/records")
async def list_records(name: str, limit: int = 500):
    """列出待上报记录，最新在前；同时回报总占用与上限。"""
    d = _store_dir(name)
    if not d.exists():
        return {"records": [], "count": 0, "total_bytes": 0, "cap_bytes": CAP_BYTES}

    items = []
    total = 0
    try:
        entries = list(d.iterdir())
    except OSError:
        entries = []

    for event_dir in entries:
        manifest = event_dir / "manifest.json"
        if not event_dir.is_dir() or not manifest.is_file():
            continue
        try:
            meta = json.loads(manifest.read_text(encoding="utf-8"))
            size = sum(path.stat().st_size for path in event_dir.iterdir() if path.is_file())
            total += size
        except Exception:
            continue
        media = meta.get("media", {})
        deliveries = meta.get("deliveries", [])
        requested = meta.get("media_requested", {})
        delivery_media = {
            str(item.get("media", ""))
            for item in deliveries
            if isinstance(item, dict) and item.get("status") != "invalid"
        }
        expects_image = bool(requested.get("image")) or "image" in delivery_media
        expects_video = bool(requested.get("video")) or "video" in delivery_media
        json_only = meta.get("delivery_mode") == "json_only" or (
            "json" in delivery_media and not expects_image and not expects_video
        )
        alarm_type = str(meta.get("alarm_type", ""))
        record_kind = str(meta.get("record_kind", "")).lower()
        if record_kind not in ("normal", "violation", "alarm"):
            record_kind = "normal" if alarm_type == "sop_normal" \
                else "violation" if alarm_type == "sop_violation" else "alarm"
        items.append({
            "id": meta.get("event_id", event_dir.name),
            "channel_id": meta.get("channel_id"),
            "alarm_type": alarm_type,
            "record_kind": record_kind,
            "media_mode": "json" if json_only else "media",
            "expects_image": expects_image,
            "expects_video": expects_video,
            "message": meta.get("message", ""),
            "trigger_count": meta.get("trigger_count", 1),
            "snapTime": meta.get("snap_time") or meta.get("trigger_unix_ms", 0),
            "ts": meta.get("ts", 0),
            "state": meta.get("state", "pending"),
            "has_image": bool(media.get("snapshot")),
            "has_raw": bool(media.get("raw")),
            "has_video": bool(media.get("video")),
            "deliveries": deliveries,
            "total_bytes": size,
        })

    items.sort(key=lambda x: x.get("ts") or 0, reverse=True)
    return {
        "records": items[: max(0, limit)],
        "count": len(items),
        "total_bytes": total,
        "cap_bytes": CAP_BYTES,
    }


@router.get("/apps/{name}/records/{rid}/json")
async def record_json(name: str, rid: str):
    """返回每条 Dify 投递按当前字段映射实际组装出的业务 JSON。"""
    if not _SAFE_ID.match(rid):
        raise HTTPException(400, "非法的记录 id")
    path = _store_dir(name) / rid / "manifest.json"
    if not path.is_file():
        raise HTTPException(404, "记录不存在或已成功投递")
    try:
        meta = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        raise HTTPException(500, "记录 JSON 读取失败")
    try:
        payloads = _dify_payloads(meta)
    except (ValueError, TypeError) as exc:
        raise HTTPException(500, f"上报字段映射无效: {exc}") from exc

    if payloads:
        primary = payloads[0]
    else:
        primary = {
            "delivery_id": "",
            "event_variable": "",
            "event_json": _business_event(meta),
        }
    return {
        "delivery_id": primary["delivery_id"],
        "event_variable": primary["event_variable"],
        "event_json": primary["event_json"],
        "payloads": payloads,
        "full_business_json": _business_event(meta),
    }


@router.get("/apps/{name}/records/{rid}/image")
async def record_image(name: str, rid: str, raw: int = 0):
    """返回某条带媒体记录的 JPEG。raw=1 取原图；原图缺失时回退到带框图。"""
    if not _SAFE_ID.match(rid):
        raise HTTPException(400, "非法的记录 id")
    d = _store_dir(name)
    event_dir = d / rid
    if event_dir.is_dir():
        p = event_dir / ("raw.jpg" if raw else "snapshot.jpg")
        if not p.is_file() and raw:
            p = event_dir / "snapshot.jpg"
        if p.is_file():
            return FileResponse(str(p), media_type="image/jpeg", headers={"Cache-Control": "no-cache"})
    raise HTTPException(404, "图片不存在(可能已补传并删除)")


@router.get("/apps/{name}/records/{rid}/video")
async def record_video(name: str, rid: str):
    if not _SAFE_ID.match(rid):
        raise HTTPException(400, "非法的记录 id")
    p = _store_dir(name) / rid / "clip.mp4"
    if not p.is_file():
        raise HTTPException(404, "视频不存在或仍在生成")
    return FileResponse(
        str(p), media_type="video/mp4",
        headers={"Cache-Control": "no-cache", "Accept-Ranges": "bytes", "Content-Disposition": "inline"},
    )


@router.post("/apps/{name}/records/{rid}/retry")
async def retry_record(name: str, rid: str):
    if not _SAFE_ID.match(rid):
        raise HTTPException(400, "非法的记录 id")
    path = _store_dir(name) / rid / "manifest.json"
    if not path.is_file():
        raise HTTPException(404, "记录不存在")
    meta = json.loads(path.read_text(encoding="utf-8"))
    for delivery in meta.get("deliveries", []):
        if delivery.get("status") != "delivered":
            delivery["status"] = "pending"
            delivery["last_error"] = ""
            delivery.pop("next_retry_unix_ms", None)
    meta["state"] = "pending"
    tmp = path.with_suffix(".json.web.tmp")
    tmp.write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)
    return {"ok": True}


@router.delete("/apps/{name}/records/{rid}")
async def delete_record(name: str, rid: str):
    if not _SAFE_ID.match(rid):
        raise HTTPException(400, "非法的记录 id")
    import shutil
    event_dir = _store_dir(name) / rid
    if not event_dir.is_dir():
        raise HTTPException(404, "记录不存在")
    shutil.rmtree(event_dir)
    return {"ok": True}
