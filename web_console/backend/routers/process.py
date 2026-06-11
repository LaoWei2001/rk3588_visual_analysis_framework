from typing import Optional

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel

from services import process_manager as pm

router = APIRouter()


class StartRequest(BaseModel):
    mode: str = "deploy"            # "deploy" | "debug"
    config: Optional[str] = None    # 指定运行的配置文件名（assets/ 下，默认 config.json）


@router.get("/apps/{name}/status")
async def app_status(name: str):
    return pm.get_status(name)


@router.post("/apps/{name}/start")
async def start_app(name: str, req: StartRequest):
    if req.mode not in ("deploy", "debug"):
        raise HTTPException(status_code=400, detail="mode must be 'deploy' or 'debug'")
    try:
        pid = pm.start_app(name, req.mode, req.config)
        return {"ok": True, "pid": pid}
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


@router.post("/apps/{name}/restart")
async def restart_app(name: str, req: StartRequest):
    pm.stop_app(name)
    try:
        pid = pm.start_app(name, req.mode, req.config)
        return {"ok": True, "pid": pid}
    except ValueError as e:
        raise HTTPException(status_code=400, detail=str(e))
    except FileNotFoundError as e:
        raise HTTPException(status_code=404, detail=str(e))
