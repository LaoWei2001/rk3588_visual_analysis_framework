"""
GET /api/apps/{name}/stream — 将 App 的 H264 RTSP 拼接画面零转码封装为碎片化 MP4。

数据路径：
  单路 FFmpeg RTSP demux → H264 stream copy → 200ms fragmented MP4 → 独立 HTTP 队列

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
import re
import shutil
import time
import uuid
from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import StreamingResponse

from services.live_stream import SharedSource, StreamPool, Viewer

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

router = APIRouter()
# 复用 uvicorn 的已配置 handler，确保 systemd journal 中能看到封装器报错。
log = logging.getLogger("uvicorn.error")


@dataclass
class _StreamSession:
    stop: asyncio.Event
    done: asyncio.Event
    source: Optional[SharedSource] = None
    viewer: Optional[Viewer] = None


# 同一视频只拉取和封装一次；浏览器会话与发送队列各自独立。
_active_streams: dict[tuple[str, str], _StreamSession] = {}
_stream_pool = StreamPool()


@dataclass
class _ReplaceLock:
    lock: asyncio.Lock
    users: int = 0  # Includes the holder and queued replacements.


_replace_locks: dict[tuple[str, str], _ReplaceLock] = {}


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

    不能只用 frag_keyframe：内置编码器的 GOP 约为 1 秒，MP4 muxer 会攒满
    整个 GOP 才向 stdout 交付，浏览器因周期性断粮表现为画面一卡一卡。固定
    200ms 切片可持续喂给 MSE；后续非关键帧切片仍属于同一连续解码时间线。
    同时在关键帧处切片，后加入的观众可直接从最近的完整 GOP 开始解码。
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
        "empty_moov+default_base_moof+frag_keyframe",
        "-frag_duration",
        "200000",
        "-flush_packets",
        "1",
        "pipe:1",
    ]


@asynccontextmanager
async def _replacement_lock(key: tuple[str, str]):
    entry = _replace_locks.setdefault(key, _ReplaceLock(asyncio.Lock()))
    # No await between lookup and increment: queued/cancelled waiters keep the
    # same lock alive, and completed viewers do not leave an unbounded registry.
    entry.users += 1
    try:
        async with entry.lock:
            yield
    finally:
        entry.users -= 1
        if entry.users == 0 and _replace_locks.get(key) is entry:
            _replace_locks.pop(key, None)


async def _replace_session(name: str, client_id: str, session: _StreamSession) -> None:
    key = (name, client_id)
    async with _replacement_lock(key):
        previous = _active_streams.get(key)
        if previous is not None:
            previous.stop.set()
            if previous.viewer is not None:
                previous.viewer.close("connection-replaced")
        _active_streams[key] = session


def _close_session(key: tuple[str, str], session: _StreamSession) -> None:
    # Removing a viewer never waits for or terminates the shared producer.
    if session.source is not None and session.viewer is not None:
        session.source.unsubscribe(session.viewer)
    if _active_streams.get(key) is session:
        _active_streams.pop(key, None)
    session.done.set()


async def _fmp4_stream(request: Request, name: str, rtsp_url: str):
    requested_id = request.query_params.get("client_id", "")
    # 老版本页面没有 client_id 时，每个请求独立，避免旧标签页抢断新版预览。
    client_id = requested_id if re.fullmatch(r"[a-zA-Z0-9_-]{1,64}", requested_id) else uuid.uuid4().hex
    key = (name, client_id)
    session = _StreamSession(stop=asyncio.Event(), done=asyncio.Event())
    started_at = time.monotonic()
    bytes_sent = 0
    end_reason = "client-disconnected"

    try:
        await _replace_session(name, client_id, session)
        session.source, session.viewer = _stream_pool.subscribe(name, rtsp_url, _build_ffmpeg_args(rtsp_url))
        log.info("[LiveStream][%s] viewer joined: viewers=%d", name, len(session.source.viewers))
        # StreamingResponse cancels this generator on disconnect. Polling the
        # request receive channel here competes with its disconnect listener.
        while not session.stop.is_set():
            chunk = await session.viewer.read()
            if chunk is None:
                end_reason = session.viewer.reason
                break
            bytes_sent += len(chunk)
            yield chunk
    finally:
        if session.stop.is_set():
            end_reason = "connection-replaced"
        _close_session(key, session)
        report = log.warning if end_reason in {"upstream-timeout", "upstream-eof", "slow-consumer"} else log.info
        report("[LiveStream][%s] closed: reason=%s seconds=%.1f bytes=%d", name,
               end_reason, time.monotonic() - started_at, bytes_sent)


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

    cause = request.query_params.get("retry_cause", "")
    if cause in {"transport-error", "read-timeout", "decoder-error", "startup-timeout", "playback-stall",
                 "buffer-timeout", "initialization-timeout"}:
        # 只记录已知事件和有限长度的播放状态；便于关联 RTSP 编码器重建前的起因。
        fields = ("retry_position_ms", "retry_ahead_ms", "retry_ready_state", "retry_paused", "retry_seeking",
                  "retry_attempt", "retry_delay_ms")
        state = {key.removeprefix("retry_"): request.query_params.get(key, "")[:32] for key in fields}
        log.warning("[LiveStream][%s] client retry: cause=%s state=%r", name, cause, state)

    return StreamingResponse(
        _fmp4_stream(request, name, rtsp_url),
        media_type="video/mp4",
        headers={
            "Cache-Control": "no-cache, no-store, must-revalidate",
            "Pragma": "no-cache",
            "X-Accel-Buffering": "no",
        },
    )
