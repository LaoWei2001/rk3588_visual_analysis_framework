from typing import Optional

from fastapi import APIRouter, HTTPException, Response
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from services import process_manager as pm
from services import runtime_state
from services.runtime_loader import load_runtime

recovery_history = load_runtime('recovery_history')
background_services = load_runtime('background_services')

router = APIRouter()


class StartRequest(BaseModel):
    config: Optional[str] = None    # 指定运行的配置文件名（assets/ 下，默认 config.json）


class AutostartRequest(BaseModel):
    enabled: bool


@router.get("/apps/{name}/status")
async def app_status(name: str, response: Response):
    response.headers["Cache-Control"] = "no-store, no-cache, must-revalidate"
    response.headers["Pragma"] = "no-cache"
    return await run_in_threadpool(pm.get_status, name)


@router.post("/apps/{name}/start")
async def start_app(name: str, req: StartRequest):
    event_id = None
    try:
        event_id = recovery_history.record('application', 'manual_start',
            '用户在程序管理页面主动启动程序', f'启动程序 {name}', source='Web 程序管理')
        pid = await run_in_threadpool(pm.start_app, name, req.config)
        recovery_history.update(event_id, 'performed', f'程序进程已启动，PID {pid}；业务状态需另行检查。')
        service_sync = await run_in_threadpool(background_services.sync_services_for_running_app)
        return {"ok": True, "pid": pid, "service_sync": service_sync}
    except pm.AppAlreadyRunningError as e:
        if event_id:
            recovery_history.update(event_id, 'failed', str(e))
        raise HTTPException(status_code=409, detail=str(e))
    except ValueError as e:
        if event_id:
            recovery_history.update(event_id, 'failed', str(e))
        raise HTTPException(status_code=400, detail=str(e))
    except FileNotFoundError as e:
        if event_id:
            recovery_history.update(event_id, 'failed', str(e))
        raise HTTPException(status_code=404, detail=str(e))
    except Exception as e:
        if event_id:
            recovery_history.update(event_id, 'failed', str(e))
        raise HTTPException(status_code=500, detail=str(e))


@router.post("/apps/{name}/stop")
async def stop_app(name: str):
    return await run_in_threadpool(_stop_app, name)


def _stop_app(name: str):
    event_id = recovery_history.safe_record('application', 'manual_stop', '用户在程序管理页面主动停止程序',
                                            f'停止程序 {name}', source='Web 程序管理', fault_recovery=False)
    try:
        result = pm.stop_app_detailed(name)
    except Exception as exc:
        if event_id:
            recovery_history.safe_update(event_id, 'failed', str(exc))
        raise
    if event_id:
        detail = result['message'] + ('；' + '；'.join(result['warnings']) if result['warnings'] else '')
        recovery_history.safe_update(event_id, 'performed', detail)
    return {"ok": not result['stopped'] or result['graceful'] is True, **result}


@router.post("/apps/{name}/autostart")
async def set_app_autostart(name: str, req: AutostartRequest):
    return await run_in_threadpool(_set_app_autostart, name, req)


def _set_app_autostart(name: str, req: AutostartRequest):
    app_dir = (pm.APPS_ROOT / name).resolve()
    try:
        app_dir.relative_to(pm.APPS_ROOT.resolve())
    except ValueError:
        raise HTTPException(status_code=400, detail="非法的程序名")
    if not app_dir.is_dir():
        raise HTTPException(status_code=404, detail=f"程序不存在: {name}")
    with pm.runtime_lock():
        settings = runtime_state.set_vision_autostart(name, req.enabled)
    return {"ok": True, **settings}


@router.post("/apps/{name}/restart")
async def restart_app(name: str, req: StartRequest):
    event_id = None
    try:
        event_id = recovery_history.record('application', 'manual_restart',
            '用户在程序管理页面主动请求重启', f'停止并重新启动程序 {name}', source='Web 程序管理')
        # start_app 会在同一把全局锁内完成同名停止和重新启动，避免重启间隙被另一 App 抢占。
        pid = await run_in_threadpool(pm.start_app, name, req.config)
        if event_id:
            recovery_history.update(event_id, 'performed', f'程序进程已启动，PID {pid}；业务状态需另行检查。')
        service_sync = await run_in_threadpool(background_services.sync_services_for_running_app)
        return {"ok": True, "pid": pid, "service_sync": service_sync}
    except pm.AppAlreadyRunningError as e:
        if event_id:
            recovery_history.update(event_id, 'failed', str(e))
        raise HTTPException(status_code=409, detail=str(e))
    except ValueError as e:
        if event_id:
            recovery_history.update(event_id, 'failed', str(e))
        raise HTTPException(status_code=400, detail=str(e))
    except FileNotFoundError as e:
        if event_id:
            recovery_history.update(event_id, 'failed', str(e))
        raise HTTPException(status_code=404, detail=str(e))
    except Exception as e:
        if event_id:
            recovery_history.update(event_id, 'failed', str(e))
        raise HTTPException(status_code=500, detail=str(e))
