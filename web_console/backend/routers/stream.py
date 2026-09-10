"""
GET /api/apps/{name}/stream — 将 App 的 H264 RTSP 拼接画面零转码封装为碎片化 MP4。

数据路径：
  FFmpeg RTSP demux → H264 stream copy → fragmented MP4 → HTTP

后端不再解码视频、不做颜色转换、也不编码 JPEG。浏览器通过 Media Source
Extensions 直接把 fMP4 交给系统 H264 解码器，CPU、内存带宽和网络带宽都只承担
必要的封装与传输开销。

Web 零转码播放明确要求 global.rtsp_codec=h264。H265 不再走隐藏的软件转码回退，
避免一份配置在不同浏览器上产生不可预测的性能和兼容性。
"""

import asyncio
import json
import logging
import os
import shutil
from contextlib import suppress
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import StreamingResponse

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

router = APIRouter()
# 复用 uvicorn 的已配置 handler，确保 systemd journal 中能看到封装器报错。
log = logging.getLogger("uvicorn.error")


@dataclass
class _StreamSession:
    stop: asyncio.Event
    done: asyncio.Event
    process: Optional[asyncio.subprocess.Process] = None


# 同一 App 只允许一个浏览器流会话。新会话会先终止旧 FFmpeg 进程并等待资源释放，
# 防止页面刷新或自动重连造成两个 RTSP 拉流进程短暂重叠。
_active_streams: dict[str, _StreamSession] = {}
_replace_locks: dict[str, asyncio.Lock] = {}


def _rtsp_info(name: str) -> tuple[str, str]:
    """读取运行配置中的板内 RTSP 地址和编码格式。"""
    port, path, codec = 8554, "/live", "h264"
    app_dir = APPS_ROOT / name
    config_name = "config.json"
    try:
        run_config = app_dir / "run.config"
        if run_config.exists():
            config_name = run_config.read_text(encoding="utf-8").strip() or config_name
        cfg_path = app_dir / "assets" / Path(config_name).name
        cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
        global_config = cfg.get("global", cfg) if isinstance(cfg, dict) else {}
        port = int(global_config.get("rtsp_port", 8554) or 8554)
        path = str(global_config.get("rtsp_path", "/live") or "/live")
        codec = str(global_config.get("rtsp_codec", "h264") or "h264").lower()
    except Exception:
        pass
    if not path.startswith("/"):
        path = "/" + path
    return f"rtsp://127.0.0.1:{port}{path}", codec


def _build_ffmpeg_args(rtsp_url: str) -> list[str]:
    """构造 H264 RTSP → fragmented MP4 的零转码管线。

    Rockchip RTSP 输出在部分 BSP/GStreamer 组合上会有首帧 DTS 缺失或
    DTS 重复。旧的 gst mp4mux 直播分片管线会因此生成浏览器无法解析的
    moof/mdat 偏移。FFmpeg 只做 demux/mux，-c:v copy 不解码也不重编码，
    同时为缺失的时间戳生成可单调的时间线。
    """
    return [
        "ffmpeg",
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "warning",
        "-rtsp_transport",
        "tcp",
        "-fflags",
        "+genpts+nobuffer",
        "-use_wallclock_as_timestamps",
        "1",
        "-i",
        rtsp_url,
        "-map",
        "0:v:0",
        "-an",
        "-c:v",
        "copy",
        "-avoid_negative_ts",
        "make_zero",
        "-f",
        "mp4",
        "-movflags",
        "frag_keyframe+empty_moov+default_base_moof",
        "-flush_packets",
        "1",
        "pipe:1",
    ]


async def _log_process_stderr(proc: asyncio.subprocess.Process, name: str) -> None:
    """持续消费 stderr，既防止子进程因管道写满阻塞，也保留现场错误。"""
    if proc.stderr is None:
        return
    while True:
        line = await proc.stderr.readline()
        if not line:
            return
        message = line.decode("utf-8", errors="replace").strip()
        if message:
            log.warning("[LiveStream][%s] ffmpeg: %s", name, message)


async def _terminate_process(proc: Optional[asyncio.subprocess.Process]) -> None:
    if proc is None or proc.returncode is not None:
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


async def _replace_session(name: str, session: _StreamSession) -> None:
    replace_lock = _replace_locks.setdefault(name, asyncio.Lock())
    async with replace_lock:
        previous = _active_streams.get(name)
        if previous is not None:
            previous.stop.set()
            await _terminate_process(previous.process)
            try:
                await asyncio.wait_for(previous.done.wait(), timeout=3.0)
            except asyncio.TimeoutError:
                pass
        _active_streams[name] = session


async def _fmp4_stream(request: Request, name: str, rtsp_url: str):
    session = _StreamSession(stop=asyncio.Event(), done=asyncio.Event())
    await _replace_session(name, session)
    stderr_task: Optional[asyncio.Task] = None

    try:
        session.process = await asyncio.create_subprocess_exec(
            *_build_ffmpeg_args(rtsp_url),
            stdin=asyncio.subprocess.DEVNULL,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        assert session.process.stdout is not None
        stderr_task = asyncio.create_task(_log_process_stderr(session.process, name))

        while not session.stop.is_set() and not await request.is_disconnected():
            try:
                chunk = await asyncio.wait_for(session.process.stdout.read(65536), timeout=15.0)
            except asyncio.TimeoutError:
                break
            if not chunk:
                break
            yield chunk
    finally:
        await _terminate_process(session.process)
        if stderr_task is not None:
            try:
                await asyncio.wait_for(stderr_task, timeout=1.0)
            except asyncio.TimeoutError:
                stderr_task.cancel()
                with suppress(asyncio.CancelledError):
                    await stderr_task
        session.process = None
        if _active_streams.get(name) is session:
            _active_streams.pop(name, None)
        session.done.set()


@router.get("/apps/{name}/stream")
async def stream_app(name: str, request: Request):
    if not (APPS_ROOT / name).exists():
        raise HTTPException(404, f"App '{name}' not found")
    if shutil.which("ffmpeg") is None:
        raise HTTPException(500, "未找到 ffmpeg，无法将 RTSP 安全封装为网页实时流")

    rtsp_url, codec = _rtsp_info(name)
    if codec not in ("h264", "avc"):
        raise HTTPException(
            409,
            "Web 零转码实时预览要求 global.rtsp_codec=h264，请修改全局 RTSP 编码格式后重启程序",
        )

    return StreamingResponse(
        _fmp4_stream(request, name, rtsp_url),
        media_type="video/mp4",
        headers={
            "Cache-Control": "no-cache, no-store, must-revalidate",
            "Pragma": "no-cache",
            "X-Accel-Buffering": "no",
        },
    )
