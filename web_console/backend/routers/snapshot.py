"""
GET /api/apps/{name}/snapshot
Grabs one frame from a video stream and returns base64 JPEG + native resolution.
Requires opencv-python-headless on the RK3588.
"""
import base64
import os
from pathlib import Path

from fastapi import APIRouter, HTTPException

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

router = APIRouter()

# USB cameras: GStreamer (createUsbDecChannel) requests NV12 at these resolutions:
#   desired_fps >= 25 → 640×480 @ 30fps
#   desired_fps >= 15 → 1280×720 @ 15fps  ← default (max_fps=15 in most configs)
#   desired_fps >= 10 → 1280×960 @ 10fps
#   desired_fps <  10 → 1920×1080 @ 5fps
# The snapshot must use the SAME resolution so ROI coordinates match the C++ pipeline.
# We default to 1280×720 (the most common case) and let the caller override via usb_width/usb_height.
USB_DEFAULT_WIDTH  = 1280
USB_DEFAULT_HEIGHT = 720


def _open_usb_cap(device: str, width: int, height: int):
    """
    Open a USB/v4l2 camera capture with explicit resolution.
    Tries device-path first, then integer index as fallback.
    Returns (cap, actual_width, actual_height) or raises RuntimeError.
    """
    import cv2  # type: ignore

    def _try_open(source):
        cap = cv2.VideoCapture(source)
        if not cap.isOpened():
            return None
        # Request the desired resolution — must match GStreamer preferred caps
        cap.set(cv2.CAP_PROP_FRAME_WIDTH,  width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        return cap

    cap = _try_open(device)
    if cap is None:
        # Fallback: extract integer index from "/dev/videoN"
        try:
            idx = int(device.replace("/dev/video", "")) if "/dev/video" in device else 0
            cap = _try_open(idx)
        except Exception:
            pass
    if cap is None or not cap.isOpened():
        raise RuntimeError(f"无法打开 USB 摄像头: {device}")
    return cap


@router.get("/apps/{name}/snapshot")
async def capture_snapshot(
    name: str,
    src_type: str = "rtsp",
    url: str = "",
    device: str = "/dev/video0",
    # Optional: caller may pass the exact USB resolution to match GStreamer caps.
    # If 0, defaults to USB_DEFAULT_WIDTH × USB_DEFAULT_HEIGHT (1280×720).
    usb_width:  int = 0,
    usb_height: int = 0,
):
    app_dir = APPS_ROOT / name
    if not app_dir.exists():
        raise HTTPException(404, f"App '{name}' not found")

    try:
        import cv2  # type: ignore
    except ImportError:
        raise HTTPException(500, "OpenCV 未安装，请执行: pip3 install opencv-python-headless")

    # Resolve source
    if src_type == "file":
        source: str = str(app_dir / url) if url and not Path(url).is_absolute() else url
    elif src_type == "usb":
        source = device if device else "/dev/video0"
    else:  # rtsp
        source = url

    if not source:
        raise HTTPException(400, "未提供视频源")

    try:
        if src_type == "usb":
            # ── USB 摄像头：必须以与 GStreamer 相同的分辨率打开 ──
            # GStreamer (createUsbDecChannel) 显式协商 NV12 分辨率；
            # 若 OpenCV 以 640×480 默认打开而 GStreamer 实际跑 1280×720，
            # 存储的 ROI 坐标会与推理管线的坐标系不一致，导致 ROI 错位。
            req_w = usb_width  if usb_width  > 0 else USB_DEFAULT_WIDTH
            req_h = usb_height if usb_height > 0 else USB_DEFAULT_HEIGHT

            cap = _open_usb_cap(source, req_w, req_h)

            # 预热：丢弃前几帧（USB 摄像头冷启动帧可能曝光不足）
            frame = None
            for _ in range(5):
                ret, f = cap.read()
                if ret:
                    frame = f
            cap.release()
        else:
            cap = cv2.VideoCapture(source)
            if not cap.isOpened():
                raise HTTPException(500, f"无法打开视频源: {source}")

            # RTSP: skip a few frames so the buffer clears; file: 1 frame is enough
            frame = None
            attempts = 5 if src_type == "rtsp" else 1
            for _ in range(attempts):
                ret, f = cap.read()
                if ret:
                    frame = f
            cap.release()

        if frame is None:
            raise HTTPException(500, "读取帧失败")

        h, w = frame.shape[:2]
        _, buf = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 80])
        img_b64 = base64.b64encode(buf.tobytes()).decode()

        return {"image": f"data:image/jpeg;base64,{img_b64}", "width": w, "height": h}

    except HTTPException:
        raise
    except Exception as exc:
        raise HTTPException(500, f"抓帧失败: {exc}") from exc
