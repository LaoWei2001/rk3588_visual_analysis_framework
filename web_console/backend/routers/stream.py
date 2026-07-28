"""
GET /api/apps/{name}/stream — 把 App 的 RTSP 拼接画面转成浏览器可播的 MJPEG。

用 GStreamer(gst-launch-1.0) 硬件解码, 取代原来的 cv2 软解:
  rtspsrc → rtph26xdepay → h26xparse → decodebin(自动选硬解/软解)
         → leaky queue(只留最新帧) → videorate(默认15 FPS)
         → videoconvert → jpegenc → multipartmux → fdsink(stdout)
后端把子进程 stdout(已是 multipart/x-mixed-replace 字节流)直接转发给 <img>。
处理速度跟不上时主动丢弃旧帧，优先保持低延迟，避免积压后出现“慢动作”。
用 decodebin 而非显式 mppvideodec: 当 C++ 侧 RTSP 源解码已占 MPP 硬解槽位时,
decodebin 自动回退软解, 避免 MPP 资源争抢导致 gst-launch 卡死 → 黑屏重连。

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
import time
from dataclasses import dataclass, field
from pathlib import Path

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import StreamingResponse

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

router = APIRouter()

@dataclass
class _StreamSession:
    stop: asyncio.Event
    done: asyncio.Event
    created_at: float = field(default_factory=time.monotonic)
    last_data_at: float = 0.0
    restart_count: int = 0


# 同一 app 同时只保留一路拉流。替换锁保证旧 gst/Mpp 解码器彻底退出后才启动新实例，
# 避免快速重试、关闭后重进时两个管线同时争抢 RTSP/MPP 资源而出现永久黑屏。
_active_streams: "dict[str, _StreamSession]" = {}
_replace_locks: "dict[str, asyncio.Lock]" = {}


def _rtsp_info(name: str):
    """从 App 当前运行配置读取 rtsp_port / rtsp_path / rtsp_codec。"""
    port, path, codec = 8554, "/live", "h264"
    app_dir = APPS_ROOT / name
    config_name = "config.json"
    try:
        run_config = app_dir / "run.config"
        if run_config.exists():
            config_name = run_config.read_text(encoding="utf-8").strip() or config_name
        cfg_path = app_dir / "assets" / Path(config_name).name
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
    """构造 RTSP → MJPEG 管线；fps<=0 时不插入限帧环节。"""
    h265 = codec in ("h265", "hevc")
    depay = "rtph265depay" if h265 else "rtph264depay"
    parse = "h265parse" if h265 else "h264parse"
    args = [
        "gst-launch-1.0", "-q",
        "rtspsrc", f"location={rtsp_url}", "protocols=tcp", "latency=100",
        "drop-on-latency=true",
        "!", depay, "!", parse, "!", "decodebin",
        "!", "queue", "max-size-buffers=1", "max-size-bytes=0",
        "max-size-time=0", "leaky=downstream",
    ]
    if fps > 0:
        args.extend([
            "!", "videorate", "drop-only=true",
            "!", f"video/x-raw,framerate={fps}/1",
        ])
    args.extend([
        "!", "videoconvert",
        "!", "jpegenc", f"quality={quality}",
        "!", "multipartmux", "boundary=frame",
        "!", "fdsink", "fd=1", "sync=false",
    ])
    return args


async def _terminate_process(proc: asyncio.subprocess.Process) -> None:
    """先优雅退出以释放 MPP，超时后强制结束。"""
    if proc.returncode is not None:
        return
    try:
        proc.terminate()
        try:
            await asyncio.wait_for(proc.wait(), timeout=2.0)
        except asyncio.TimeoutError:
            proc.kill()
            await proc.wait()
    except ProcessLookupError:
        pass


async def _mjpeg_stream(request: Request, name: str, rtsp_url: str, codec: str, fps: int):
    session = _StreamSession(stop=asyncio.Event(), done=asyncio.Event())
    replace_lock = _replace_locks.setdefault(name, asyncio.Lock())

    # 串行替换：必须等旧 gst 释放 MPP/RTSP 后，才能启动本次转发。
    async with replace_lock:
        prev = _active_streams.get(name)
        if prev is not None:
            prev.stop.set()
            try:
                await asyncio.wait_for(prev.done.wait(), timeout=8.0)
            except asyncio.TimeoutError:
                # 旧请求失去调度等极端情况下不能永久卡住新请求。
                pass
        _active_streams[name] = session

    args = _build_gst_args(rtsp_url, codec, fps)
    try:
        # HTTP/MJPEG 连接保持不变；内部 gst 因断流、解码器异常或 5 秒无输出时自动重建。
        while not session.stop.is_set() and not await request.is_disconnected():
            proc = await asyncio.create_subprocess_exec(
                *args,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.DEVNULL,
            )
            session.restart_count += 1
            produced_data = False
            try:
                assert proc.stdout is not None
                while not session.stop.is_set() and not await request.is_disconnected():
                    try:
                        chunk = await asyncio.wait_for(proc.stdout.read(65536), timeout=5.0)
                    except asyncio.TimeoutError:
                        break  # 管线卡住：结束本实例并在外层重建。
                    if not chunk:
                        break  # gst 退出或 RTSP 断开。
                    produced_data = True
                    session.last_data_at = time.monotonic()
                    yield chunk
            finally:
                await _terminate_process(proc)

            if session.stop.is_set() or await request.is_disconnected():
                break
            await asyncio.sleep(0.5 if produced_data else 1.0)
    finally:
        if _active_streams.get(name) is session:
            _active_streams.pop(name, None)
        session.done.set()


@router.get("/apps/{name}/stream")
async def stream_app(name: str, request: Request, fps: int = 15):
    if not (APPS_ROOT / name).exists():
        raise HTTPException(404, f"App '{name}' not found")
    if shutil.which("gst-launch-1.0") is None:
        raise HTTPException(500, "未找到 gst-launch-1.0（需要 gstreamer1.0-tools）")

    url, codec = _rtsp_info(name)
    # fps<=0 表示不额外限帧；正数保留兼容的 1~25 FPS 主动限帧模式。
    fps = 0 if int(fps) <= 0 else min(int(fps), 25)
    return StreamingResponse(
        _mjpeg_stream(request, name, url, codec, fps),
        media_type="multipart/x-mixed-replace; boundary=frame",
        headers={
            "Cache-Control": "no-cache, no-store, must-revalidate",
            "Pragma": "no-cache",
            "X-Accel-Buffering": "no",
        },
    )


@router.get("/apps/{name}/stream-health")
async def stream_health(name: str):
    """Return the MJPEG transport heartbeat for the browser stall watchdog."""
    if not (APPS_ROOT / name).exists():
        raise HTTPException(404, f"App '{name}' not found")

    session = _active_streams.get(name)
    if session is None:
        return {"active": False, "last_data_age_ms": None, "restart_count": 0}

    age_ms = None
    if session.last_data_at > 0:
        age_ms = int((time.monotonic() - session.last_data_at) * 1000)
    return {
        "active": not session.stop.is_set(),
        "last_data_age_ms": age_ms,
        "restart_count": session.restart_count,
    }
