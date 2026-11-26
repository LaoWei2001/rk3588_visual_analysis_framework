from typing import Optional

from fastapi import APIRouter, HTTPException, Response
from pydantic import BaseModel
from starlette.concurrency import run_in_threadpool

from services import process_manager as pm
from services import runtime_state
from services.runtime_loader import load_runtime

recovery_history = load_runtime('recovery_history')
background_services = load_runtime('background_services')
manual_start = load_runtime('manual_start').start

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
    return await _start_request(name, req)


async def _start_request(name: str, req: StartRequest, restart: bool = False):
    try:
        return await run_in_threadpool(manual_start, name, req.config, 'Web 程序管理', restart)
    except pm.AppAlreadyRunningError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc


@router.post("/apps/{name}/stop")
async def stop_app(name: str):
    return await run_in_threadpool(_stop_app, name)


def _stop_app(name: str):
    event_id = recovery_history.safe_record('application', 'manual_stop', '用户在程序管理页面主动停止程序',
                                            f'停止程序 {name}', source='Web 程序管理', fault_recovery=False)
    try:
        result = pm.stop_app_detailed(name)
    except pm.CommandLineAppRunningError as exc:
        if event_id:
            recovery_history.safe_update(event_id, 'failed', str(exc))
        raise HTTPException(status_code=409, detail=str(exc)) from exc
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
    return await _start_request(name, req, restart=True)
