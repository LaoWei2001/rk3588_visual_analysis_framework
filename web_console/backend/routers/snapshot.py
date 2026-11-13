"""HTTP adapter for one-frame previews used by the configuration editor."""
import os
from pathlib import Path

from fastapi import APIRouter, HTTPException
from starlette.concurrency import run_in_threadpool

from services.app_paths import application_directory
from services.frame_snapshot import capture_frame

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
router = APIRouter()


@router.get("/apps/{name}/snapshot")
async def capture_snapshot(
    name: str,
    src_type: str = "rtsp",
    url: str = "",
    device: str = "/dev/video81",
    usb_width: int = 0,
    usb_height: int = 0,
):
    app_dir = application_directory(APPS_ROOT, name)
    try:
        return await run_in_threadpool(
            capture_frame, app_dir, src_type, url, device, usb_width, usb_height,
        )
    except ImportError as exc:
        raise HTTPException(500, "OpenCV 未安装，请执行: pip3 install opencv-python-headless") from exc
    except ValueError as exc:
        raise HTTPException(400, str(exc)) from exc
    except Exception as exc:
        raise HTTPException(500, f"抓帧失败: {exc}") from exc
