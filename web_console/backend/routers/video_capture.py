"""独立视频采集 API。

接口只负责参数验证和调度 ``services.video_capture_manager``，不读取视觉 App
配置，也不复用告警录像或实时画面路由。
"""
import asyncio
from typing import Literal

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import StreamingResponse
from pydantic import BaseModel, Field

from services.video_capture_manager import (
    VideoCaptureBusyError,
    VideoCaptureError,
    VideoCaptureInputError,
    VideoCaptureRuntimeError,
    capture_manager,
    list_usb_devices,
    storage_snapshot,
)


router = APIRouter(prefix="/video-capture", tags=["video-capture"])


class CaptureSourceRequest(BaseModel):
    source_type: Literal["rtsp", "usb"] = "rtsp"
    rtsp_url: str = Field(default="", max_length=4096)
    usb_device: str = Field(default="/dev/video0", max_length=64)
    usb_width: int = Field(default=0, ge=0, le=16384)
    usb_height: int = Field(default=0, ge=0, le=16384)


class RecordingStartRequest(CaptureSourceRequest):
    save_path: str = Field(min_length=1, max_length=4096)
    max_file_size_mb: int = Field(ge=64, le=1024 * 1024)


def _raise_http(error: VideoCaptureError) -> None:
    if isinstance(error, VideoCaptureBusyError):
        raise HTTPException(status_code=409, detail=str(error)) from error
    if isinstance(error, VideoCaptureInputError):
        raise HTTPException(status_code=400, detail=str(error)) from error
    if isinstance(error, VideoCaptureRuntimeError):
        raise HTTPException(status_code=500, detail=str(error)) from error
    raise HTTPException(status_code=500, detail=str(error)) from error


@router.get("/devices")
async def capture_devices():
    return {"devices": await asyncio.to_thread(list_usb_devices)}


@router.get("/storage")
async def capture_storage(path: str = Query(min_length=1, max_length=4096)):
    try:
        return await asyncio.to_thread(storage_snapshot, path)
    except VideoCaptureError as exc:
        _raise_http(exc)


@router.get("/status")
async def capture_status():
    return await asyncio.to_thread(capture_manager.status)


@router.post("/preview/start")
async def start_capture_preview(req: CaptureSourceRequest):
    try:
        return await asyncio.to_thread(capture_manager.start_preview, req.model_dump())
    except VideoCaptureError as exc:
        _raise_http(exc)


@router.post("/preview/stop")
async def stop_capture_preview():
    try:
        return await asyncio.to_thread(capture_manager.stop_preview)
    except VideoCaptureError as exc:
        _raise_http(exc)


@router.get("/preview/stream")
async def capture_preview_stream():
    try:
        stream = capture_manager.open_preview_stream()
    except VideoCaptureError as exc:
        _raise_http(exc)
    return StreamingResponse(
        stream,
        media_type="multipart/x-mixed-replace; boundary=frame",
        headers={
            "Cache-Control": "no-cache, no-store, must-revalidate",
            "Pragma": "no-cache",
            "X-Accel-Buffering": "no",
        },
    )


@router.post("/recordings/start")
async def start_capture_recording(req: RecordingStartRequest):
    try:
        return await asyncio.to_thread(
            capture_manager.start_recording,
            req.model_dump(include={
                "source_type", "rtsp_url", "usb_device", "usb_width", "usb_height",
            }),
            req.save_path,
            req.max_file_size_mb * 1024 * 1024,
        )
    except VideoCaptureError as exc:
        _raise_http(exc)


@router.post("/recordings/stop")
async def stop_capture_recording():
    try:
        return await asyncio.to_thread(capture_manager.stop_recording, "manual")
    except VideoCaptureError as exc:
        _raise_http(exc)
