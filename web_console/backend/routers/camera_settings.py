"""直连海康摄像头的独立配置 API。"""
from __future__ import annotations

import asyncio
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException, Query, Request
from pydantic import BaseModel, Field

from routers.network_settings import require_network_admin
from services import camera_manager
from services.camera_web_proxy import CameraWebProxyError, camera_web_proxy


router = APIRouter()


def _http_error(exc: Exception, status_code: int = 400) -> HTTPException:
    if isinstance(exc, PermissionError):
        return HTTPException(503, "摄像头发现/配置需要 root、CAP_NET_RAW 和 CAP_NET_ADMIN 权限")
    if isinstance(exc, camera_manager.CameraManagerError):
        return HTTPException(status_code, str(exc))
    if isinstance(exc, CameraWebProxyError):
        return HTTPException(503, str(exc))
    return HTTPException(500, f"摄像头管理操作失败：{exc}")


class CameraDiscoveryRequest(BaseModel):
    interface: str = Field(min_length=1, max_length=15, pattern=r"^[A-Za-z0-9_.:-]+$")
    timeout_ms: int = Field(default=3000, ge=200, le=15000)


class CameraConfigurationRequest(BaseModel):
    interface: str = Field(min_length=1, max_length=15, pattern=r"^[A-Za-z0-9_.:-]+$")
    camera_ip: str = Field(min_length=7, max_length=15)
    prefix_length: int = Field(ge=1, le=30)
    camera_mac: str = Field(default="", max_length=17)
    model: str = Field(default="", max_length=160)
    serial: str = Field(default="", max_length=160)
    http_port: int = Field(default=80, ge=1, le=65535)
    rtsp_port: int = Field(default=554, ge=1, le=65535)
    http_port_inferred: bool = False
    rtsp_port_inferred: bool = False


def _management_peer(request: Request) -> str:
    return request.client.host if request.client else ""


@router.get("/system/camera")
async def get_camera_settings(include_status: bool = Query(default=True)):
    return await asyncio.to_thread(camera_manager.snapshot, include_status=include_status)


@router.post("/system/camera/discover")
async def discover_cameras(req: CameraDiscoveryRequest,
                           _admin: None = Depends(require_network_admin)):
    try:
        cameras = await asyncio.to_thread(
            camera_manager.discover_cameras, req.interface, req.timeout_ms / 1000.0,
        )
        return {"interface": req.interface, "cameras": cameras}
    except Exception as exc:
        raise _http_error(exc)


@router.post("/system/camera/plan")
async def plan_camera_configuration(req: CameraConfigurationRequest, request: Request,
                                    _admin: None = Depends(require_network_admin)):
    try:
        return await asyncio.to_thread(
            camera_manager.plan_configuration, req.model_dump(), _management_peer(request),
        )
    except Exception as exc:
        raise _http_error(exc)


@router.put("/system/camera/config")
async def apply_camera_configuration(req: CameraConfigurationRequest, request: Request,
                                     _admin: None = Depends(require_network_admin)):
    try:
        configuration = await asyncio.to_thread(
            camera_manager.apply_configuration, req.model_dump(), _management_peer(request),
        )
        status: Optional[dict] = None
        status_error: Optional[str] = None
        try:
            status = await asyncio.to_thread(camera_manager.configuration_status, configuration)
        except camera_manager.CameraManagerError as exc:
            status_error = str(exc)
        return {
            "ok": True,
            "configuration": configuration,
            "status": status,
            "status_error": status_error,
        }
    except Exception as exc:
        raise _http_error(exc)


@router.delete("/system/camera/config")
async def remove_camera_configuration(_admin: None = Depends(require_network_admin)):
    try:
        await asyncio.to_thread(camera_manager.remove_configuration)
        return {"ok": True}
    except Exception as exc:
        raise _http_error(exc)


@router.get("/system/camera/status")
async def get_camera_status():
    try:
        return {"status": await asyncio.to_thread(camera_manager.configuration_status)}
    except Exception as exc:
        raise _http_error(exc)


@router.post("/system/camera/web-session")
async def open_camera_web_session(request: Request,
                                  _admin: None = Depends(require_network_admin)):
    """为当前 Web 管理端临时开放透明摄像头 HTTP 代理。"""
    try:
        session = await asyncio.to_thread(
            camera_web_proxy.start_session, _management_peer(request),
        )
        return {"ok": True, **session}
    except Exception as exc:
        raise _http_error(exc)
