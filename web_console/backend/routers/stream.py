"""
GET /api/apps/{name}/stream — 把 App 的 RTSP 拼接画面转成浏览器可播的 MJPEG。

用 GStreamer(gst-launch-1.0) 硬件解码, 取代原来的 cv2 软解:
  rtspsrc → rtph26xdepay → h26xparse → mppvideodec(MPP 硬解)
         → videorate(限帧) → videoconvert → jpegenc → multipartmux → fdsink(stdout)
后端把子进程 stdout(已是 multipart/x-mixed-replace 字节流)直接转发给 <img>。
H264/H265 解码走 MPP 硬件块, 单路监看 CPU 大幅低于 cv2 软解。

资源释放(防 CPU 累积):
  - 异步转发 stdout; 每轮检查 request.is_disconnected() 与 single-flight 停止标志,
    关弹窗/刷新/被同 app 新流取代 → 立即终止 gst-launch 子进程, 不泄漏。
鉴权: ?token= 查询参数 (main.py 的 auth_middleware 已放行)。

依赖: gst-launch-1.0 (gstreamer1.0-tools) + mppvideodec(瑞芯微 GStreamer 插件), 板子均已具备。
"""
import asyncio
import json
import os
import shutil
from pathlib import Path

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import StreamingResponse

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

router = APIRouter()

# 同一 app 同时只保留一路拉流: app 名 → 当前流的停止事件。
_active_streams: "dict[str, asyncio.Event]" = {}


def _rtsp_info(name: str):
    """从 App 的 config.json 读 rtsp_port / rtsp_path / rtsp_codec → (url, codec)。"""
    port, path, codec = 8554, "/live", "h264"
    cfg_path = APPS_ROOT / name / "assets" / "config.json"
    try:
        cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        g = cfg.get("global", cfg) if isinstance(cfg, dict) else {}
        port = int(g.get("rtsp_port", 8554) or 8554)
        path = str(g.get("rtsp_path", "/live") or "/live")
        codec = str(g.get("rtsp_codec", "h264") or "h264").lower()
    except Exception:
        pass
    if not path.startswith("/"):
        path = "/" + path
    return f"rtsp://127.0.0.1:{port}{path}", codec


def _build_gst_args(rtsp_url: str, codec: str, fps: int, quality: int = 75):
    """显式用 mppvideodec 强制硬解(不靠 decodebin 选, 避免被软解 avdec 抢)。"""
    h265 = codec in ("h265", "hevc")
    depay = "rtph265depay" if h265 else "rtph264depay"
    parse = "h265parse" if h265 else "h264parse"
    return [
        "gst-launch-1.0", "-q",
        "rtspsrc", f"location={rtsp_url}", "protocols=tcp", "latency=100",
        "!", depay, "!", parse, "!", "mppvideodec",
        "!", "videorate", "!", f"video/x-raw,framerate={fps}/1",
        "!", "videoconvert",
        "!", "jpegenc", f"quality={quality}",
        "!", "multipartmux", "boundary=frame",
        "!", "fdsink", "fd=1",
    ]


async def _mjpeg_stream(request: Request, name: str, rtsp_url: str, codec: str, fps: int):
    # 单飞: 通知该 app 之前的流停止, 自己登记为当前流
    prev = _active_streams.get(name)
    if prev is not None:
        prev.set()
    my_stop = asyncio.Event()
    _active_streams[name] = my_stop

    args = _build_gst_args(rtsp_url, codec, fps)
    proc = await asyncio.create_subprocess_exec(
        *args,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.DEVNULL,
    )
    try:
        assert proc.stdout is not None
        while True:
            # 关弹窗/刷新(断开) 或 被同 app 新流取代 → 立刻停止
            if my_stop.is_set() or await request.is_disconnected():
                break
            try:
                chunk = await asyncio.wait_for(proc.stdout.read(65536), timeout=5.0)
            except asyncio.TimeoutError:
                continue        # 期间无数据 → 回去检查断开/停止标志
            if not chunk:
                break           # 子进程结束(流断/管线出错) → 前端 <img> onError
            yield chunk
    finally:
        if _active_streams.get(name) is my_stop:
            _active_streams.pop(name, None)
        # 终止 gst-launch: 先 SIGTERM 让 MPP 干净释放, 2s 超时再 SIGKILL
        try:
            proc.terminate()
            try:
                await asyncio.wait_for(proc.wait(), timeout=2.0)
            except asyncio.TimeoutError:
                proc.kill()
                await proc.wait()
        except ProcessLookupError:
            pass


@router.get("/apps/{name}/stream")
async def stream_app(name: str, request: Request, fps: int = 10):
    if not (APPS_ROOT / name).exists():
        raise HTTPException(404, f"App '{name}' not found")
    if shutil.which("gst-launch-1.0") is None:
        raise HTTPException(500, "未找到 gst-launch-1.0（需要 gstreamer1.0-tools）")

    url, codec = _rtsp_info(name)
    fps = max(1, min(int(fps), 25))
    return StreamingResponse(
        _mjpeg_stream(request, name, url, codec, fps),
        media_type="multipart/x-mixed-replace; boundary=frame",
        headers={
            "Cache-Control": "no-cache, no-store, must-revalidate",
            "Pragma": "no-cache",
            "Connection": "close",
        },
    )
