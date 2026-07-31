from typing import Optional

from fastapi import APIRouter, HTTPException, Response
from pydantic import BaseModel

from services import process_manager as pm
from services import runtime_state

router = APIRouter()


class StartRequest(BaseModel):
    mode: str = "deploy"            # "deploy" | "debug"
    config: Optional[str] = None    # 指定运行的配置文件名（assets/ 下，默认 config.json）


class AutostartRequest(BaseModel):
    enabled: bool


def _sync_background_services():
    # 延迟导入，避免 process/services 两个路由模块初始化时循环依赖。
    from routers.services import sync_services_for_running_app
    return sync_services_for_running_app()


@router.get("/apps/{name}/status")
async def app_status(name: str, response: Response):
    response.headers["Cache-Control"] = "no-store, no-cache, must-revalidate"
    response.headers["Pragma"] = "no-cache"
    return pm.get_status(name)


@router.post("/apps/{name}/start")
async def start_app(name: str, req: StartRequest):
    if req.mode not in ("deploy", "debug"):
        raise HTTPException(status_code=400, detail="mode must be 'deploy' or 'debug'")
    try:
        pid = pm.start_app(name, req.mode, req.config)
        service_sync = _sync_background_services()
        return {"ok": True, "pid": pid, "service_sync": service_sync}
    except pm.AppAlreadyRunningError as e:
        raise HTTPException(status_code=409, detail=str(e))
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e))
    except FileNotFoundError as e:
        raise HTTPException(status_code=404, detail=str(e))
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@router.post("/apps/{name}/stop")
async def stop_app(name: str):
    pm.stop_app(name)
    return {"ok": True}


@router.post("/apps/{name}/autostart")
async def set_app_autostart(name: str, req: AutostartRequest):
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
    if req.mode not in ("deploy", "debug"):
        raise HTTPException(status_code=400, detail="mode must be 'deploy' or 'debug'")
    try:
        # start_app 会在同一把全局锁内完成同名停止和重新启动，避免重启间隙被另一 App 抢占。
        pid = pm.start_app(name, req.mode, req.config)
        service_sync = _sync_background_services()
        return {"ok": True, "pid": pid, "service_sync": service_sync}
    except pm.AppAlreadyRunningError as e:
        raise HTTPException(status_code=409, detail=str(e))
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e))
    except FileNotFoundError as e:
        raise HTTPException(status_code=404, detail=str(e))
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))
