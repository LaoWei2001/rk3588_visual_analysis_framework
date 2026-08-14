import asyncio
from typing import List

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from services import storage_manager


router = APIRouter()


class StorageSettingsRequest(BaseModel):
    auto_cleanup: bool = False
    retention_days: int = Field(default=30, ge=1, le=3650)
    max_event_store_gb: float = Field(default=1.0, ge=0.1, le=1000.0)
    min_free_gb: float = Field(default=1.0, ge=0.0, le=1000.0)


class RootCleanupRequest(BaseModel):
    targets: List[str] = Field(min_length=1, max_length=6)


@router.get("/system/storage")
async def get_storage_settings():
    return await asyncio.to_thread(storage_manager.storage_snapshot)


@router.put("/system/storage")
async def set_storage_settings(req: StorageSettingsRequest):
    storage_manager.write_settings(req.model_dump())
    return {"ok": True, **await asyncio.to_thread(storage_manager.storage_snapshot)}


@router.post("/system/storage/cleanup")
async def cleanup_storage():
    result = await asyncio.to_thread(storage_manager.cleanup_now)
    return {"ok": True, "cleanup": result, **await asyncio.to_thread(storage_manager.storage_snapshot)}


@router.post("/system/storage/root-cleanup")
async def cleanup_root_storage(req: RootCleanupRequest):
    try:
        result = await asyncio.to_thread(storage_manager.cleanup_root_targets, req.targets)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return {
        "ok": not result["errors"],
        "root_cleanup": result,
        **await asyncio.to_thread(storage_manager.storage_snapshot),
    }


async def maintenance_loop() -> None:
    """控制台存活期间定期执行策略；C++ 侧仍独立执行容量/剩余空间兜底。"""
    while True:
        await asyncio.sleep(300)
        if storage_manager.read_settings()["auto_cleanup"]:
            await asyncio.to_thread(storage_manager.cleanup_now)
        await asyncio.sleep(3300)
