"""Authenticated HTTP adapter for the shared system service manager."""
import sys

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from services.runtime_loader import load_runtime

_core = load_runtime("background_services")
router = APIRouter()


class AutostartReq(BaseModel):
    enabled: bool


async def _call(function, *args):
    try:
        return await run_in_threadpool(function, *args)
    except _core.ServiceError as exc:
        raise HTTPException(status_code=exc.status_code, detail=exc.detail) from exc


@router.get("/services")
async def list_services():
    return await _call(lambda: [
        {"key": key, "label": meta["label"], "unit": meta["unit"],
         "scope": meta.get("scope", "app"), **_core._status(key)}
        for key, meta in _core.MANAGED.items()
    ])


@router.post("/services/{key}/autostart")
async def set_service_autostart(key: str, req: AutostartReq):
    return await _call(_core.configure_autostart, key, req.enabled)


@router.post("/services/{key}/{action}")
async def control_service(key: str, action: str):
    if key not in _core.MANAGED or action not in ('start', 'restart'):
        return await _call(_core.control, key, action)
    history = load_runtime('recovery_history')
    unit = _core.MANAGED[key]['unit']
    event_id = await run_in_threadpool(history.record, 'system', 'manual_service_control',
        '用户在系统服务页请求启动或重启服务', f'{action} {unit}')
    try:
        result = await _call(_core.control, key, action)
    except Exception as exc:
        await run_in_threadpool(history.update, event_id, 'failed', str(exc))
        raise
    await run_in_threadpool(history.update, event_id, 'performed', '服务控制命令已完成；实际运行状态请查看服务状态。')
    return result


@router.get("/services/{key}/logs")
async def service_logs(key: str, lines: int = 200):
    return await _call(_core.logs, key, lines)


# Compatibility for established Python imports. The independent system service
# imports rk3588_runtime directly, without this HTTP adapter or its dependencies.
_core.router = router
_core.AutostartReq = AutostartReq
_core.list_services = list_services
_core.set_service_autostart = set_service_autostart
_core.control_service = control_service
_core.service_logs = service_logs
sys.modules[__name__] = _core
