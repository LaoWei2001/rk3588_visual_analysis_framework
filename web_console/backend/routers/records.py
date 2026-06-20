"""
GET  /api/apps/{name}/records              列出本地"发件箱"里待上报的告警(平台还没收到的)
GET  /api/apps/{name}/records/{rid}/image  取某条告警的截图(带框图; ?raw=1 取原图)

数据由 C++ 报警时落盘到 <app>/alarm_store/ (env ALARM_STORE_DIR 可覆盖),
Python 上报微服务补传成功后会删除 —— 所以这里只会看到"还没传上去"的那些。
"""
import json
import os
import re
from pathlib import Path

from fastapi import APIRouter, HTTPException
from fastapi.responses import FileResponse

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
CAP_BYTES = 100 * 1024 * 1024  # 与 C++ 记录器 ALARM_STORE_CAP_BYTES 一致(仅用于页面展示)
_SAFE_ID = re.compile(r"^[A-Za-z0-9._-]+$")

router = APIRouter()


def _store_dir(name: str) -> Path:
    """该 App 的发件箱目录。env ALARM_STORE_DIR 为全局覆盖, 否则 <app>/alarm_store。"""
    override = os.environ.get("ALARM_STORE_DIR")
    if override:
        return Path(override)
    return APPS_ROOT / name / "alarm_store"


@router.get("/apps/{name}/records")
async def list_records(name: str, limit: int = 500):
    """列出待上报告警, 最新在前; 同时回报总占用与上限(给页面显示)。"""
    d = _store_dir(name)
    if not d.exists():
        return {"records": [], "count": 0, "total_bytes": 0, "cap_bytes": CAP_BYTES}

    items = []
    total = 0
    try:
        entries = list(d.iterdir())
    except OSError:
        entries = []

    for f in entries:
        try:
            if not f.is_file():
                continue
            total += f.stat().st_size
        except OSError:
            continue
        if f.suffix != ".json":
            continue
        try:
            meta = json.loads(f.read_text(encoding="utf-8"))
        except Exception:
            continue
        items.append({
            "id": f.stem,
            "camera_id": meta.get("camera_id"),
            "alarm_type": meta.get("alarm_type", ""),
            "snapTime": meta.get("snapTime", ""),
            "ts": meta.get("ts", 0),
            "has_raw": bool(meta.get("img_raw")),
        })

    items.sort(key=lambda x: x.get("ts") or 0, reverse=True)
    return {
        "records": items[: max(0, limit)],
        "count": len(items),
        "total_bytes": total,
        "cap_bytes": CAP_BYTES,
    }


@router.get("/apps/{name}/records/{rid}/image")
async def record_image(name: str, rid: str, raw: int = 0):
    """返回某条告警的 JPEG。raw=1 取原图; 原图缺失时回退到带框图。"""
    if not _SAFE_ID.match(rid):
        raise HTTPException(400, "非法的记录 id")
    d = _store_dir(name)
    candidates = []
    if raw:
        candidates.append(d / f"{rid}_raw.jpg")
    candidates.append(d / f"{rid}.jpg")
    for p in candidates:
        if p.is_file():
            return FileResponse(str(p), media_type="image/jpeg",
                                headers={"Cache-Control": "no-cache"})
    raise HTTPException(404, "图片不存在(可能已补传并删除)")
