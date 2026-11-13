"""Blocking single-frame capture and encoding, independent of HTTP routing."""
import base64
from pathlib import Path

# Match the C++ USB capture defaults so editor ROI coordinates stay aligned.
USB_DEFAULT_WIDTH = 1280
USB_DEFAULT_HEIGHT = 720


def _open_usb_capture(cv2, device: str, width: int, height: int):
    def try_open(source):
        cap = cv2.VideoCapture(source)
        try:
            if cap.isOpened():
                cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
                cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
                return cap
        except Exception:
            cap.release()
            raise
        cap.release()
        return None

    cap = try_open(device)
    if cap is None:
        try:
            index = int(device.replace("/dev/video", "")) if "/dev/video" in device else 0
        except ValueError:
            index = None
        if index is not None:
            cap = try_open(index)
    if cap is None:
        raise RuntimeError(f"无法打开 USB 摄像头: {device}")
    return cap


def capture_frame(app_dir: Path, src_type: str, url: str, device: str,
                  usb_width: int, usb_height: int) -> dict:
    """Capture outside the event loop; always release the device on read errors."""
    import cv2

    if src_type == "file":
        source = str(app_dir / url) if url and not Path(url).is_absolute() else url
    elif src_type == "usb":
        source = device or "/dev/video81"
    else:
        source = url
    if not source:
        raise ValueError("未提供视频源")

    if src_type == "usb":
        cap = _open_usb_capture(
            cv2, source,
            usb_width if usb_width > 0 else USB_DEFAULT_WIDTH,
            usb_height if usb_height > 0 else USB_DEFAULT_HEIGHT,
        )
    else:
        cap = cv2.VideoCapture(source)
    try:
        if not cap.isOpened():
            raise RuntimeError(f"无法打开视频源: {source}")
        frame = None
        for _ in range(5 if src_type in ("rtsp", "usb") else 1):
            ok, candidate = cap.read()
            if ok:
                frame = candidate
    finally:
        cap.release()
    if frame is None:
        raise RuntimeError("读取帧失败")
    height, width = frame.shape[:2]
    ok, encoded = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, 80])
    if not ok:
        raise RuntimeError("JPEG 编码失败")
    image = base64.b64encode(encoded.tobytes()).decode()
    return {"image": f"data:image/jpeg;base64,{image}", "width": width, "height": height}
