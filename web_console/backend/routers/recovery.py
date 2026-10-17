"""Web management of independently running device recovery services."""
from typing import Any, Dict

from fastapi import APIRouter, Depends, HTTPException
from starlette.concurrency import run_in_threadpool
from services.runtime_loader import load_runtime
from routers.network_settings import require_network_admin

management = load_runtime('management')
policy = load_runtime('recovery_policy')
device_info = load_runtime('device_info')
performance = load_runtime('performance')
router = APIRouter()


async def call(function, *args):
    try:
        return await run_in_threadpool(function, *args)
    except policy.PolicyConflict as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc


@router.get('/system/recovery')
async def get_recovery():
    return await call(management.snapshot)


@router.get('/system/recovery/history')
async def get_recovery_history():
    return await call(management.history_snapshot)


@router.get('/system/device-info')
async def get_device_info():
    return await call(device_info.snapshot)


@router.put('/system/recovery')
async def save_recovery(payload: Dict[str, Any]):
    if set(payload) != {'policy', 'revision'}:
        raise HTTPException(status_code=400, detail='必须同时提交策略和读取时的版本号')
    if not isinstance(payload['revision'], str):
        raise HTTPException(status_code=400, detail='策略版本号格式不正确')
    await call(policy.write, payload['policy'], payload['revision'])
    return await call(management.snapshot)


@router.post('/system/recovery/apply')
async def apply_recovery():
    return await call(management.apply_to_current_app)


@router.put('/system/recovery/application')
async def save_application_recovery(payload: Dict[str, Any]):
    if set(payload) != {'policy', 'revision'} or not isinstance(payload['revision'], str):
        raise HTTPException(status_code=400, detail='请同时提交程序恢复设置和读取时的版本号')
    return await call(management.save_application_policy, payload['policy'], payload['revision'])


@router.put('/system/recovery/system')
async def save_system_recovery(payload: Dict[str, Any], _admin: None = Depends(require_network_admin)):
    if set(payload) != {'policy', 'revision'} or not isinstance(payload['revision'], str):
        raise HTTPException(status_code=400, detail='请同时提交系统恢复设置和读取时的版本号')
    return await call(management.save_system_policy, payload['policy'], payload['revision'])


@router.post('/system/recovery/application/apply')
async def apply_application_recovery(payload: Dict[str, Any]):
    if (set(payload) != {'revision', 'app'} or not isinstance(payload['revision'], str)
            or not isinstance(payload['app'], str) or not payload['app']):
        raise HTTPException(status_code=400, detail='请提交设置版本号和当前程序名称')
    return await call(management.apply_to_current_app, payload['revision'], payload['app'])


@router.post('/system/recovery/agent/{action}')
async def control_agent(action: str):
    return await call(management.control, action)


@router.get('/system/recovery/network')
async def get_network_recovery():
    return await call(management.network_snapshot)


@router.put('/system/recovery/network')
async def save_network_recovery(payload: Dict[str, Any], _admin: None = Depends(require_network_admin)):
    if set(payload) != {'policy', 'revision'} or not isinstance(payload['revision'], str):
        raise HTTPException(status_code=400, detail='必须提交网络恢复策略及读取时的版本号')
    return await call(management.save_network_policy, payload['policy'], payload['revision'])


@router.put('/system/device-info/performance')
async def save_performance(payload: Dict[str, Any], _admin: None = Depends(require_network_admin)):
    return await call(performance.save, payload)


@router.post('/system/device-info/performance/reset')
async def reset_performance(payload: Dict[str, Any], _admin: None = Depends(require_network_admin)):
    return await call(performance.save, payload, True)
