"""独立的视频素材采集管理器。

该模块不依赖视觉 App、算法管线或事件发件箱。RTSP H264 输入直接复用摄像头
压缩码流写入 MP4；RTSP H265 输入通过 RK3588 MPP 硬件解码和 H264 硬件编码
标准化后写入 MP4，避免不同摄像头 H265/H265+ 参数集、时间戳和 hvc1 封装差异。
USB MJPEG/NV12/YUYV 统一转换为 I420，再以受控码率编码为高质量 H264。USB
路径保留 x264 软件编码，以正确处理 MJPEG full-range 色彩和非 16 对齐高度，
同时避免 MJPEG 原帧直存造成文件过大。Web 只负责控制进程和消费低帧率预览。
"""
from __future__ import annotations

import json
import os
import re
import shutil
import signal
import stat
import subprocess
import threading
import time
from collections import deque
from datetime import datetime
from pathlib import Path
from typing import Any, Deque, Dict, Iterable, List, Optional, Tuple
from urllib.parse import urlsplit, urlunsplit


GSTREAMER = os.environ.get("VIDEO_CAPTURE_GSTREAMER", "gst-launch-1.0")
FFPROBE = os.environ.get("VIDEO_CAPTURE_FFPROBE", "ffprobe")
V4L2_CTL = os.environ.get("VIDEO_CAPTURE_V4L2_CTL", "v4l2-ctl")

_DEFAULT_ALLOWED_ROOTS = "/userdata:/mnt:/media:/opt"
ALLOWED_ROOTS: Tuple[Path, ...] = tuple(
    Path(value).resolve()
    for value in os.environ.get("VIDEO_CAPTURE_ALLOWED_ROOTS", _DEFAULT_ALLOWED_ROOTS).split(os.pathsep)
    if value.strip()
)

MIN_FREE_RESERVE_BYTES = int(os.environ.get(
    "VIDEO_CAPTURE_MIN_FREE_BYTES", str(512 * 1024 * 1024)
))
FREE_RESERVE_RATIO = float(os.environ.get("VIDEO_CAPTURE_FREE_RESERVE_RATIO", "0.05"))
FINALIZE_MARGIN_BYTES = int(os.environ.get(
    "VIDEO_CAPTURE_FINALIZE_MARGIN_BYTES", str(8 * 1024 * 1024)
))
SIZE_STOP_MARGIN_BYTES = int(os.environ.get(
    "VIDEO_CAPTURE_SIZE_STOP_MARGIN_BYTES", str(4 * 1024 * 1024)
))
PREVIEW_MAX_WIDTH = 960
PREVIEW_MAX_HEIGHT = 540
PREVIEW_MAX_FPS = 5

_USB_DEVICE_RE = re.compile(r"^/dev/video\d+$")
_MOUNT_ESCAPE_RE = re.compile(r"\\([0-7]{3})")
_FAT32_TYPES = {"vfat", "msdos", "fat"}
_FAT32_MAX_FILE_BYTES = 4 * 1024**3 - 1
_USB_RAW_FORMATS = {
    "NV12": "NV12",
    "YUYV": "YUY2",
}
_USB_FORMAT_PREFERENCE = {
    # UVC 设备的原始 NV12/YUYV 经常携带矛盾的色彩信息，优先摄像头自身的
    # MJPEG，并用软件 jpegdec 正确处理色彩范围和非 16 对齐高度。
    "MJPG": 3,
    "NV12": 2,
    "YUYV": 1,
}


class VideoCaptureError(RuntimeError):
    """视频采集基础错误。"""


class VideoCaptureInputError(VideoCaptureError):
    """用户输入或采集源不合法。"""


class VideoCaptureBusyError(VideoCaptureError):
    """已有互斥的视频采集操作。"""


class VideoCaptureRuntimeError(VideoCaptureError):
    """外部采集工具或设备运行失败。"""


def _decode_mount_field(value: str) -> str:
    return _MOUNT_ESCAPE_RE.sub(lambda match: chr(int(match.group(1), 8)), value)


def _mount_details(path: Path) -> Tuple[str, str]:
    """返回 path 对应的最长匹配挂载点和文件系统类型。"""
    selected_mount = "/"
    selected_fs = "unknown"
    try:
        lines = Path("/proc/self/mountinfo").read_text(encoding="utf-8").splitlines()
    except OSError:
        return selected_mount, selected_fs

    resolved = str(path.resolve())
    for line in lines:
        try:
            before, after = line.split(" - ", 1)
            fields = before.split()
            after_fields = after.split()
            mount_point = _decode_mount_field(fields[4])
            fs_type = after_fields[0]
            common = os.path.commonpath((resolved, mount_point))
        except (IndexError, ValueError):
            continue
        if common == mount_point and len(mount_point) >= len(selected_mount):
            selected_mount = mount_point
            selected_fs = fs_type
    return selected_mount, selected_fs


def _allowed_path(path: Path) -> bool:
    for root in ALLOWED_ROOTS:
        try:
            resolved_root = root.resolve(strict=True)
        except OSError:
            continue
        if path == resolved_root or resolved_root in path.parents:
            return True
    return False


def resolve_capture_directory(raw_path: str) -> Path:
    value = (raw_path or "").strip()
    if not value:
        raise VideoCaptureInputError("请选择录像保存路径")
    candidate = Path(value)
    if not candidate.is_absolute():
        raise VideoCaptureInputError("录像保存路径必须是板端绝对路径")
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as exc:
        raise VideoCaptureInputError(f"录像保存路径不存在：{value}") from exc
    if not resolved.is_dir():
        raise VideoCaptureInputError("录像保存路径不是目录")
    if not _allowed_path(resolved):
        roots = "、".join(str(path) for path in ALLOWED_ROOTS)
        raise VideoCaptureInputError(f"录像路径必须位于允许的目录下：{roots}")
    return resolved


def storage_snapshot(raw_path: str) -> Dict[str, Any]:
    """读取指定目录所在文件系统的真实可用空间和安全录像上限。"""
    path = resolve_capture_directory(raw_path)
    try:
        values = os.statvfs(path)
    except OSError as exc:
        raise VideoCaptureInputError(f"无法读取路径存储空间：{exc}") from exc

    block_size = values.f_frsize or values.f_bsize
    total_bytes = int(values.f_blocks * block_size)
    free_bytes = int(values.f_bfree * block_size)
    available_bytes = int(values.f_bavail * block_size)
    used_bytes = max(0, total_bytes - free_bytes)
    reserve_bytes = max(MIN_FREE_RESERVE_BYTES, int(total_bytes * FREE_RESERVE_RATIO))
    safe_available_bytes = max(0, available_bytes - reserve_bytes)
    mount_point, filesystem = _mount_details(path)
    filesystem_max_file_bytes: Optional[int] = (
        _FAT32_MAX_FILE_BYTES if filesystem.lower() in _FAT32_TYPES else None
    )
    max_recording_file_bytes = max(0, safe_available_bytes - FINALIZE_MARGIN_BYTES)
    if filesystem_max_file_bytes is not None:
        max_recording_file_bytes = min(max_recording_file_bytes, filesystem_max_file_bytes)

    return {
        "path": str(path),
        "mount_point": mount_point,
        "filesystem": filesystem,
        "writable": os.access(path, os.W_OK | os.X_OK),
        "total_bytes": total_bytes,
        "used_bytes": used_bytes,
        "free_bytes": free_bytes,
        "available_bytes": available_bytes,
        "reserve_bytes": reserve_bytes,
        "safe_available_bytes": safe_available_bytes,
        "filesystem_max_file_bytes": filesystem_max_file_bytes,
        "max_recording_file_bytes": max_recording_file_bytes,
        "allowed_roots": [str(root) for root in ALLOWED_ROOTS],
    }


def validate_recording_capacity(snapshot: Dict[str, Any], max_file_size_bytes: int) -> None:
    if not snapshot.get("writable"):
        raise VideoCaptureInputError("录像保存路径不可写")
    if max_file_size_bytes <= 0:
        raise VideoCaptureInputError("单个 MP4 最大大小必须大于 0")
    filesystem_limit = snapshot.get("filesystem_max_file_bytes")
    if filesystem_limit is not None and max_file_size_bytes > int(filesystem_limit):
        raise VideoCaptureInputError(
            "当前文件系统不支持这么大的单个文件，请降低文件上限或改用 ext4"
        )
    allowed = int(snapshot.get("max_recording_file_bytes", 0))
    if max_file_size_bytes > allowed:
        raise VideoCaptureInputError(
            "所选路径的安全可用空间小于单个 MP4 最大大小，请降低上限或更换路径"
        )


def _write_probe(directory: Path) -> None:
    probe = directory / f".video_capture_write_probe_{os.getpid()}_{time.time_ns()}"
    descriptor: Optional[int] = None
    try:
        descriptor = os.open(str(probe), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.write(descriptor, b"ok")
        os.fsync(descriptor)
    except OSError as exc:
        raise VideoCaptureInputError(f"录像保存路径写入测试失败：{exc}") from exc
    finally:
        if descriptor is not None:
            os.close(descriptor)
        try:
            probe.unlink()
        except OSError:
            pass


def _is_usb_capture_device(path: Path) -> bool:
    sys_device = Path("/sys/class/video4linux") / path.name / "device"
    try:
        device_target = str(sys_device.resolve(strict=True)).lower()
    except OSError:
        return False
    if "usb" not in device_target:
        return False
    if shutil.which(V4L2_CTL) is None:
        return True
    try:
        result = subprocess.run(
            [V4L2_CTL, "--device", str(path), "--all"],
            capture_output=True, text=True, timeout=3,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return result.returncode == 0 and "Format Video Capture:" in result.stdout


def _parse_v4l2_modes(output: str) -> List[Dict[str, Any]]:
    """解析 ``v4l2-ctl --list-formats-ext`` 的离散采集模式。"""
    modes: List[Dict[str, Any]] = []
    pixel_format = ""
    current: Optional[Dict[str, Any]] = None
    for line in output.splitlines():
        format_match = re.search(r"\[\d+\]:\s*'([^']+)'", line)
        if format_match:
            pixel_format = format_match.group(1).strip().upper()
            current = None
            continue
        size_match = re.search(r"Size:\s*Discrete\s+(\d+)x(\d+)", line)
        if size_match and pixel_format:
            current = {
                "pixel_format": pixel_format,
                "width": int(size_match.group(1)),
                "height": int(size_match.group(2)),
                "fps": 0.0,
            }
            modes.append(current)
            continue
        fps_match = re.search(r"\(([0-9]+(?:\.[0-9]+)?)\s+fps\)", line)
        if fps_match and current is not None:
            current["fps"] = max(float(current["fps"]), float(fps_match.group(1)))
    return modes


def _query_usb_modes(device: Path) -> List[Dict[str, Any]]:
    if shutil.which(V4L2_CTL) is None:
        return []
    try:
        result = subprocess.run(
            [V4L2_CTL, "--device", str(device), "--list-formats-ext"],
            capture_output=True, text=True, timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    if result.returncode != 0:
        return []
    return _parse_v4l2_modes(result.stdout)


def _usb_resolution_options(modes: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    resolutions: Dict[Tuple[int, int], Dict[str, Any]] = {}
    for mode in modes:
        if str(mode.get("pixel_format")) not in _USB_FORMAT_PREFERENCE:
            continue
        key = (int(mode["width"]), int(mode["height"]))
        option = resolutions.setdefault(key, {
            "width": key[0],
            "height": key[1],
            "max_fps": 0.0,
        })
        option["max_fps"] = max(float(option["max_fps"]), float(mode.get("fps") or 0))
    return sorted(
        resolutions.values(),
        key=lambda item: (int(item["width"]) * int(item["height"]), int(item["width"])),
        reverse=True,
    )


def list_usb_devices() -> List[Dict[str, Any]]:
    devices: List[Dict[str, Any]] = []
    paths = sorted(Path("/dev").glob("video[0-9]*"), key=lambda item: int(item.name[5:]))
    for path in paths:
        try:
            mode = path.stat().st_mode
        except OSError:
            continue
        if not stat.S_ISCHR(mode) or not _is_usb_capture_device(path):
            continue
        name_file = Path("/sys/class/video4linux") / path.name / "name"
        try:
            label = name_file.read_text(encoding="utf-8").strip()
        except OSError:
            label = path.name
        modes = _query_usb_modes(path)
        devices.append({
            "device": str(path),
            "label": label or path.name,
            "readable": os.access(path, os.R_OK),
            "resolutions": _usb_resolution_options(modes),
        })
    return devices


def _parse_fraction(value: Any) -> float:
    text = str(value or "")
    if "/" in text:
        left, right = text.split("/", 1)
        try:
            denominator = float(right)
            return float(left) / denominator if denominator else 0.0
        except ValueError:
            return 0.0
    try:
        return float(text)
    except ValueError:
        return 0.0


def _redact_rtsp_url(value: str) -> str:
    try:
        parsed = urlsplit(value)
        host = parsed.hostname or ""
        if ":" in host and not host.startswith("["):
            host = f"[{host}]"
        if parsed.port:
            host = f"{host}:{parsed.port}"
        return urlunsplit((parsed.scheme, host, parsed.path, parsed.query, ""))
    except (TypeError, ValueError):
        return "rtsp://***"


def _normalize_source(source: Dict[str, Any]) -> Dict[str, Any]:
    source_type = str(source.get("source_type", "")).strip().lower()
    if source_type == "rtsp":
        url = str(source.get("rtsp_url", "")).strip()
        try:
            parsed = urlsplit(url)
        except ValueError as exc:
            raise VideoCaptureInputError("RTSP 地址格式无效") from exc
        if parsed.scheme.lower() not in ("rtsp", "rtsps") or not parsed.hostname:
            raise VideoCaptureInputError("RTSP 地址必须以 rtsp:// 或 rtsps:// 开头")
        return {
            "source_type": "rtsp", "rtsp_url": url, "usb_device": "",
            "usb_width": 0, "usb_height": 0,
        }
    if source_type == "usb":
        device = str(source.get("usb_device", "")).strip()
        if not _USB_DEVICE_RE.fullmatch(device):
            raise VideoCaptureInputError("USB 设备必须是 /dev/videoN")
        try:
            width = int(source.get("usb_width") or 0)
            height = int(source.get("usb_height") or 0)
        except (TypeError, ValueError) as exc:
            raise VideoCaptureInputError("USB 分辨率格式无效") from exc
        if (width == 0) != (height == 0):
            raise VideoCaptureInputError("USB 分辨率的宽和高必须同时设置")
        if width < 0 or height < 0 or width > 16384 or height > 16384:
            raise VideoCaptureInputError("USB 分辨率超出允许范围")
        return {
            "source_type": "usb", "rtsp_url": "", "usb_device": device,
            "usb_width": width, "usb_height": height,
        }
    raise VideoCaptureInputError("视频来源只能是 RTSP 或 USB")


def _probe_rtsp(source: Dict[str, Any]) -> Dict[str, Any]:
    if shutil.which(FFPROBE) is None:
        raise VideoCaptureRuntimeError("未找到 ffprobe，无法识别 RTSP 编码格式")
    command = [
        FFPROBE,
        "-v", "error",
        "-rtsp_transport", "tcp",
        "-select_streams", "v:0",
        "-show_entries", "stream=codec_name,width,height,avg_frame_rate,bit_rate",
        "-of", "json",
        source["rtsp_url"],
    ]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=12)
    except subprocess.TimeoutExpired as exc:
        raise VideoCaptureRuntimeError("连接 RTSP 视频源超时") from exc
    except OSError as exc:
        raise VideoCaptureRuntimeError(f"无法运行 ffprobe：{exc}") from exc
    if result.returncode != 0:
        detail = (result.stderr or "无法读取视频流").strip().splitlines()[-1]
        detail = detail.replace(source["rtsp_url"], _redact_rtsp_url(source["rtsp_url"]))
        raise VideoCaptureRuntimeError(f"RTSP 视频源连接失败：{detail}")
    try:
        streams = json.loads(result.stdout).get("streams", [])
        stream = streams[0]
    except (AttributeError, IndexError, TypeError, ValueError, json.JSONDecodeError) as exc:
        raise VideoCaptureRuntimeError("RTSP 地址中没有可识别的视频流") from exc
    codec = str(stream.get("codec_name", "")).lower()
    if codec == "hevc":
        codec = "h265"
    if codec not in ("h264", "h265"):
        raise VideoCaptureRuntimeError("MP4 原码流采集当前只支持 H264 或 H265 RTSP 视频")
    return {
        "codec": codec,
        "width": int(stream.get("width") or 0),
        "height": int(stream.get("height") or 0),
        "fps": round(_parse_fraction(stream.get("avg_frame_rate")), 2),
        "bitrate": int(stream.get("bit_rate") or 0),
    }


def _current_usb_mode(device: Path) -> Dict[str, Any]:
    current: Dict[str, Any] = {"width": 0, "height": 0, "pixel_format": "", "fps": 0.0}
    if shutil.which(V4L2_CTL) is None:
        return current
    try:
        fmt = subprocess.run(
            [V4L2_CTL, "--device", str(device), "--get-fmt-video"],
            capture_output=True, text=True, timeout=5,
        )
        size_match = re.search(r"Width/Height\s*:\s*(\d+)\s*/\s*(\d+)", fmt.stdout)
        format_match = re.search(r"Pixel Format\s*:\s*'([^']+)'", fmt.stdout)
        if size_match:
            current["width"], current["height"] = int(size_match.group(1)), int(size_match.group(2))
        if format_match:
            current["pixel_format"] = format_match.group(1).strip().upper()
        parm = subprocess.run(
            [V4L2_CTL, "--device", str(device), "--get-parm"],
            capture_output=True, text=True, timeout=5,
        )
        fps_match = re.search(r"Frames per second:\s*([0-9.]+)", parm.stdout)
        if fps_match:
            current["fps"] = float(fps_match.group(1))
    except (OSError, subprocess.TimeoutExpired, ValueError):
        pass
    return current


def _select_usb_mode(
    modes: List[Dict[str, Any]], width: int, height: int,
) -> Optional[Dict[str, Any]]:
    candidates = [
        mode for mode in modes
        if int(mode["width"]) == width and int(mode["height"]) == height
        and str(mode["pixel_format"]) in _USB_FORMAT_PREFERENCE
    ]
    if not candidates:
        return None
    return max(candidates, key=lambda mode: (
        float(mode.get("fps") or 0),
        _USB_FORMAT_PREFERENCE.get(str(mode.get("pixel_format")), 0),
    ))


def _probe_usb(source: Dict[str, Any]) -> Dict[str, Any]:
    device = Path(source["usb_device"])
    try:
        mode = device.stat().st_mode
    except OSError as exc:
        raise VideoCaptureRuntimeError(f"USB 设备不存在：{device}") from exc
    if not stat.S_ISCHR(mode) or not os.access(device, os.R_OK):
        raise VideoCaptureRuntimeError(f"USB 视频设备不可读：{device}")
    if not _is_usb_capture_device(device):
        raise VideoCaptureRuntimeError(f"所选节点不是可采集画面的 USB 视频设备：{device}")

    requested_width = int(source.get("usb_width") or 0)
    requested_height = int(source.get("usb_height") or 0)
    modes = _query_usb_modes(device)
    selected: Optional[Dict[str, Any]] = None
    if requested_width and requested_height:
        selected = _select_usb_mode(modes, requested_width, requested_height)
        if selected is None and modes:
            supported = "、".join(
                f"{item['width']}×{item['height']}"
                for item in _usb_resolution_options(modes)
            )
            if not supported:
                raise VideoCaptureInputError("USB 设备没有可用的 MJPEG、NV12 或 YUYV 采集格式")
            raise VideoCaptureInputError(
                f"USB 设备不支持 {requested_width}×{requested_height}，可选：{supported}"
            )

    current = _current_usb_mode(device)
    if selected is None:
        width = requested_width or int(current["width"])
        height = requested_height or int(current["height"])
        selected = _select_usb_mode(modes, width, height)
    if selected is None:
        selected = {
            "width": requested_width or int(current["width"]),
            "height": requested_height or int(current["height"]),
            "pixel_format": str(current["pixel_format"]),
            "fps": float(current["fps"]),
        }
    return {
        "codec": "mjpeg" if str(selected.get("pixel_format") or "") == "MJPG" else "h264",
        "input_codec": "usb",
        "input_format": str(selected.get("pixel_format") or ""),
        "width": int(selected.get("width") or 0),
        "height": int(selected.get("height") or 0),
        "fps": round(float(selected.get("fps") or 0), 2),
        "bitrate": 0,
    }


def probe_source(source: Dict[str, Any]) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    normalized = _normalize_source(source)
    if shutil.which(GSTREAMER) is None:
        raise VideoCaptureRuntimeError("未找到 gst-launch-1.0")
    if normalized["source_type"] == "rtsp":
        return normalized, _probe_rtsp(normalized)
    return normalized, _probe_usb(normalized)


def _preview_dimensions(width: int, height: int) -> Tuple[int, int]:
    if width <= 0 or height <= 0:
        return PREVIEW_MAX_WIDTH, PREVIEW_MAX_HEIGHT
    scale = min(1.0, PREVIEW_MAX_WIDTH / width, PREVIEW_MAX_HEIGHT / height)
    preview_width = max(16, int(width * scale))
    preview_height = max(16, int(height * scale))
    return preview_width - preview_width % 2, preview_height - preview_height % 2


def _jpeg_preview_tail(width: int, height: int) -> List[str]:
    preview_width, preview_height = _preview_dimensions(width, height)
    return [
        "videorate", "drop-only=true", f"max-rate={PREVIEW_MAX_FPS}", "!",
        "videoscale", "!",
        "videoconvert", "!",
        f"video/x-raw,format=I420,width={preview_width},height={preview_height}", "!",
        "jpegenc", "quality=75", "!",
        "fdsink", "fd=1", "sync=false",
    ]


def _rtsp_ingest(source: Dict[str, Any], probe: Dict[str, Any]) -> List[str]:
    codec = probe["codec"]
    encoding = "H264" if codec == "h264" else "H265"
    depay = "rtph264depay" if codec == "h264" else "rtph265depay"
    parser = "h264parse" if codec == "h264" else "h265parse"
    return [
        "rtspsrc", f"location={source['rtsp_url']}", "protocols=tcp", "latency=100",
        "buffer-mode=1", "drop-on-latency=true", "!",
        f"application/x-rtp,media=video,encoding-name={encoding}", "!",
        depay, "!", parser, "config-interval=-1",
    ]


def _mp4_record_tail(codec: str, output_path: Path) -> List[str]:
    parser = "h264parse" if codec == "h264" else "h265parse"
    stream_format = "avc" if codec == "h264" else "hvc1"
    caps = f"video/x-{codec},stream-format={stream_format},alignment=au"
    return [
        parser, "config-interval=-1", "!", caps, "!",
        "mp4mux", "fragment-duration=1000", "!",
        "filesink", f"location={output_path}", "sync=false",
    ]


def _pipeline_error_summary(lines: Iterable[str], fallback: str) -> str:
    """返回首个真正的管线错误，避免 PAUSE/TEARDOWN 次生错误遮住根因。"""
    values = [str(line).strip() for line in lines if str(line).strip()]
    blocks: List[List[str]] = []
    current: List[str] = []
    for line in values:
        is_error_start = line.startswith("ERROR:") or line.startswith("错误：")
        if is_error_start:
            if current:
                blocks.append(current)
            current = [line]
        elif current and len(current) < 8:
            current.append(line)
    if current:
        blocks.append(current)

    def is_rtsp_shutdown_noise(block: List[str]) -> bool:
        text = "\n".join(block)
        return (
            "gst_rtspsrc_pause" in text
            and ("Could not send message" in text or "Received end-of-file" in text)
        )

    meaningful = [block for block in blocks if not is_rtsp_shutdown_noise(block)]
    if meaningful:
        return "；".join(meaningful[0])
    if blocks:
        return "；".join(blocks[-1])
    return "；".join(values[-4:]) if values else fallback


def _gst_framerate(value: Any) -> str:
    try:
        fps = float(value)
    except (TypeError, ValueError):
        fps = 0.0
    if fps <= 0:
        return ""
    rounded = round(fps)
    if abs(fps - rounded) < 0.01:
        return f"{rounded}/1"
    return f"{round(fps * 1000)}/1000"


def _usb_ingest(source: Dict[str, Any], probe: Dict[str, Any]) -> List[str]:
    command = ["v4l2src", f"device={source['usb_device']}", "do-timestamp=true", "!"]
    width = int(probe.get("width") or 0)
    height = int(probe.get("height") or 0)
    pixel_format = str(probe.get("input_format") or "").upper()
    fields: List[str] = []
    if width > 0:
        fields.append(f"width={width}")
    if height > 0:
        fields.append(f"height={height}")
    framerate = _gst_framerate(probe.get("fps"))
    if framerate:
        fields.append(f"framerate={framerate}")
    suffix = f",{','.join(fields)}" if fields else ""

    if pixel_format == "MJPG":
        # 当前板端 MPP JPEG 解码器对 1080/360 等非 16 对齐高度会暴露填充行，
        # 并可能丢失 JPEG full-range 色彩信息，表现为绿条、偏色和花屏。
        return command + [f"image/jpeg{suffix}", "!", "jpegparse", "!", "jpegdec", "!"]
    raw_format = _USB_RAW_FORMATS.get(pixel_format)
    if raw_format:
        return command + [f"video/x-raw,format={raw_format}{suffix}", "!"]
    # 无 v4l2-ctl 或设备未报告格式时保持兼容，由 GStreamer 自动协商。
    return command + ["decodebin", "!"]


def _h264_encoder_rates(probe: Dict[str, Any]) -> Tuple[int, int, int]:
    """按像素量分配高质量 H264 目标码率，同时限制长期文件增长。"""
    width = max(1, int(probe.get("width") or 0))
    height = max(1, int(probe.get("height") or 0))
    fps = min(60.0, max(1.0, float(probe.get("fps") or 25.0)))
    target = min(20_000_000, max(2_500_000, int(width * height * fps * 0.15)))
    minimum = max(1_500_000, int(target * 0.6))
    maximum = min(25_000_000, int(target * 1.25))
    return target, minimum, maximum


def _mpp_h264_encoder(probe: Dict[str, Any], profile: str = "main") -> List[str]:
    fps = min(60.0, max(1.0, float(probe.get("fps") or 25.0)))
    width = max(1, int(probe.get("width") or 0))
    height = max(1, int(probe.get("height") or 0))
    target, minimum, maximum = _h264_encoder_rates(probe)
    if width > 1920 or height > 1080:
        level = "5.2" if fps > 30 else "5.1"
    else:
        level = "4.2" if fps > 30 else "4.1"
    return [
        "mpph264enc", "rc-mode=vbr", f"profile={profile}", f"level={level}",
        f"bps={target}", f"bps-min={minimum}", f"bps-max={maximum}",
        "qp-init=24", "qp-min=18", "qp-max=32",
        f"gop={max(1, int(round(fps)))}", "header-mode=each-idr",
    ]


def _x264_encoder(probe: Dict[str, Any]) -> List[str]:
    fps = min(60.0, max(1.0, float(probe.get("fps") or 25.0)))
    target, _, _ = _h264_encoder_rates(probe)
    return [
        "x264enc", "pass=cbr", "speed-preset=superfast", "tune=zerolatency",
        "bframes=0", "qp-min=18", "qp-max=32",
        f"key-int-max={max(1, int(round(fps)))}",
        f"bitrate={max(1, target // 1000)}",
    ]


def build_preview_args(source: Dict[str, Any], probe: Dict[str, Any]) -> List[str]:
    command = [GSTREAMER, "-q", "-e"]
    if source["source_type"] == "rtsp":
        command += _rtsp_ingest(source, probe)
        command += ["!", "mppvideodec", "!"]
    else:
        command += _usb_ingest(source, probe)
    command += _jpeg_preview_tail(int(probe.get("width") or 0), int(probe.get("height") or 0))
    return command


def build_record_args(
    source: Dict[str, Any], probe: Dict[str, Any], output_path: Path,
) -> List[str]:
    command = [GSTREAMER, "-q", "-e"]
    width = int(probe.get("width") or 0)
    height = int(probe.get("height") or 0)
    if source["source_type"] == "rtsp":
        codec = str(probe["codec"])
        command += _rtsp_ingest(source, probe)
        if codec == "h265":
            # 部分摄像头的 H265/H265+ 可以正常解码预览，但其参数集、时间戳或
            # NAL 排列不能被 GStreamer 1.18 的 h265parse/mp4mux 稳定转换为
            # hvc1。先用 MPP 解码，再硬件编码成标准 H264；录像和预览复用同一
            # 次解码，既消除厂商码流差异，也不会把转码负担放到 CPU 上。
            command += ["!", "mppvideodec", "format=NV12", "!", "tee", "name=capture_decoded"]
            command += ["capture_decoded.", "!", "queue", "!"]
            command += _mpp_h264_encoder(probe)
            command += ["!"]
            command += _mp4_record_tail("h264", output_path)
            command += [
                "capture_decoded.", "!",
                "queue", "leaky=downstream", "max-size-buffers=2", "max-size-bytes=0", "max-size-time=0", "!",
            ]
            command += _jpeg_preview_tail(width, height)
            return command

        command += ["!", "tee", "name=capture_encoded"]
        command += ["capture_encoded.", "!", "queue", "!"]
        command += _mp4_record_tail(codec, output_path)
        command += [
            "capture_encoded.", "!",
            "queue", "leaky=downstream", "max-size-buffers=2", "max-size-bytes=0", "max-size-time=0", "!",
            "mppvideodec", "!",
        ]
        command += _jpeg_preview_tail(width, height)
        return command

    # MJPEG 先用 jpegdec 正确处理 full-range；NV12/YUYV 直接进入 videoconvert。
    # 两类输入都标准化为紧凑 I420，再由 x264 编码。这里不能使用 mpph264enc：
    # 部分 UVC 分辨率（例如高度 1080/360）不是 16 对齐，MPP 可能把 stride
    # 填充区编码成底部绿条，且 MJPEG full-range 还可能产生明显色偏。
    # 录像与低帧率预览共享一次解码/颜色转换，文件仍比逐帧 MJPEG 小得多。
    command += _usb_ingest(source, probe)
    command += [
        "videoconvert", "!", "video/x-raw,format=I420", "!",
        "tee", "name=capture_raw",
        "capture_raw.", "!", "queue", "!",
    ]
    command += _x264_encoder(probe)
    command += ["!"]
    command += _mp4_record_tail("h264", output_path)
    command += [
        "capture_raw.", "!",
        "queue", "leaky=downstream", "max-size-buffers=2", "max-size-bytes=0", "max-size-time=0", "!",
    ]
    command += _jpeg_preview_tail(width, height)
    return command


def _timestamp_stem(now: Optional[datetime] = None) -> str:
    value = now or datetime.now().astimezone()
    return value.strftime("%Y%m%d_%H%M%S_%f")


class VideoCaptureManager:
    """单实例采集器。预览和录像进程互斥，录像不依赖浏览器连接。"""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._preview_condition = threading.Condition(self._lock)
        self._operation_lock = threading.RLock()
        self._process: Optional[subprocess.Popen] = None
        self._generation = 0
        self._state = "idle"
        self._source: Optional[Dict[str, Any]] = None
        self._probe: Optional[Dict[str, Any]] = None
        self._started_unix_ms: Optional[int] = None
        self._record_started_monotonic: Optional[float] = None
        self._working_path: Optional[Path] = None
        self._output_path: Optional[Path] = None
        self._save_directory: Optional[Path] = None
        self._max_file_size_bytes = 0
        self._stop_reason: Optional[str] = None
        self._error: Optional[str] = None
        self._stderr: Deque[str] = deque(maxlen=120)
        self._latest_preview_frame: Optional[bytes] = None
        self._preview_sequence = 0
        self._record_done = threading.Event()

    def _source_summary(self) -> Optional[Dict[str, str]]:
        if not self._source:
            return None
        if self._source["source_type"] == "rtsp":
            return {
                "source_type": "rtsp",
                "label": _redact_rtsp_url(self._source["rtsp_url"]),
            }
        return {"source_type": "usb", "label": self._source["usb_device"]}

    def status(self) -> Dict[str, Any]:
        with self._lock:
            process = self._process
            state = self._state
            working_path = self._working_path
            output_path = self._output_path
            save_directory = self._save_directory
            started_unix_ms = self._started_unix_ms
            record_started = self._record_started_monotonic
            result: Dict[str, Any] = {
                "state": state,
                "recording": state in ("recording", "stopping"),
                "previewing": process is not None and state in ("previewing", "recording", "stopping"),
                "source": self._source_summary(),
                "probe": dict(self._probe) if self._probe else None,
                "started_unix_ms": started_unix_ms,
                "elapsed_seconds": round(max(0.0, time.monotonic() - record_started), 1)
                if record_started is not None and state in ("recording", "stopping") else 0.0,
                "output_path": str(output_path) if output_path else None,
                "file_size_bytes": 0,
                "max_file_size_bytes": self._max_file_size_bytes,
                "stop_reason": self._stop_reason,
                "error": self._error,
                "process_alive": process is not None and process.poll() is None,
            }
        file_path = working_path if working_path and working_path.exists() else output_path
        if file_path:
            try:
                result["file_size_bytes"] = file_path.stat().st_size
            except OSError:
                pass
        if save_directory:
            try:
                result["storage"] = storage_snapshot(str(save_directory))
            except VideoCaptureError:
                result["storage"] = None
        else:
            result["storage"] = None
        return result

    def _reset_process_state(self) -> None:
        self._process = None
        self._preview_condition.notify_all()

    def _spawn(self, command: List[str]) -> subprocess.Popen:
        try:
            return subprocess.Popen(
                command,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
            )
        except OSError as exc:
            raise VideoCaptureRuntimeError(f"无法启动视频采集管线：{exc}") from exc

    def _drain_stderr(self, process: subprocess.Popen, generation: int) -> None:
        if process.stderr is None:
            return
        try:
            for raw in iter(process.stderr.readline, b""):
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                with self._lock:
                    if generation != self._generation:
                        continue
                    if self._source and self._source["rtsp_url"]:
                        line = line.replace(
                            self._source["rtsp_url"], _redact_rtsp_url(self._source["rtsp_url"]),
                        )
                    self._stderr.append(line)
        except (OSError, ValueError):
            pass

    def _drain_preview_stdout(self, process: subprocess.Popen, generation: int) -> None:
        """持续排空预览 stdout，并只发布完整 JPEG。

        即使浏览器关闭也必须继续读取，否则管道写满 stdout 后会卡住预览分支，
        进而妨碍录像进程接收 EOS 和完成 MP4。
        """
        if process.stdout is None:
            return
        pending = bytearray()
        try:
            descriptor = process.stdout.fileno()
            while process.poll() is None:
                chunk = os.read(descriptor, 65536)
                if not chunk:
                    break
                pending.extend(chunk)
                while True:
                    start = pending.find(b"\xff\xd8")
                    if start < 0:
                        if len(pending) > 1:
                            del pending[:-1]
                        break
                    end = pending.find(b"\xff\xd9", start + 2)
                    if end < 0:
                        if start > 0:
                            del pending[:start]
                        if len(pending) > 16 * 1024 * 1024:
                            pending.clear()
                        break
                    frame = bytes(pending[start:end + 2])
                    del pending[:end + 2]
                    with self._preview_condition:
                        if generation != self._generation or self._process is not process:
                            return
                        self._latest_preview_frame = frame
                        self._preview_sequence += 1
                        self._preview_condition.notify_all()
        except (OSError, ValueError):
            pass
        finally:
            with self._preview_condition:
                self._preview_condition.notify_all()

    def _recent_pipeline_error(self, fallback: str) -> str:
        with self._lock:
            lines = list(self._stderr)
        return _pipeline_error_summary(lines, fallback)

    @staticmethod
    def _terminate_process(process: subprocess.Popen, graceful: bool, timeout: float = 12.0) -> None:
        if process.poll() is not None:
            return
        try:
            process.send_signal(signal.SIGINT if graceful else signal.SIGTERM)
            process.wait(timeout=timeout)
            return
        except (ProcessLookupError, subprocess.TimeoutExpired):
            pass
        if process.poll() is None:
            try:
                process.terminate()
                process.wait(timeout=3)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                if process.poll() is None:
                    try:
                        process.kill()
                        process.wait(timeout=2)
                    except (ProcessLookupError, subprocess.TimeoutExpired):
                        pass

    def _watch_preview(self, process: subprocess.Popen, generation: int) -> None:
        return_code = process.wait()
        with self._lock:
            if generation != self._generation or self._process is not process:
                return
            self._reset_process_state()
            if self._state == "previewing":
                self._state = "error" if return_code != 0 else "idle"
                if return_code != 0:
                    self._error = self._recent_pipeline_error("视频预览已中断")

    def start_preview(self, source_input: Dict[str, Any]) -> Dict[str, Any]:
        with self._operation_lock:
            with self._lock:
                if self._state in ("recording", "stopping"):
                    return self.status()
            self.stop_preview()
            source, probe = probe_source(source_input)
            command = build_preview_args(source, probe)
            process = self._spawn(command)
            with self._lock:
                self._generation += 1
                generation = self._generation
                self._process = process
                self._state = "previewing"
                self._source = source
                self._probe = probe
                self._started_unix_ms = None
                self._record_started_monotonic = None
                self._working_path = None
                self._save_directory = None
                self._max_file_size_bytes = 0
                self._stop_reason = None
                self._error = None
                self._stderr.clear()
                self._latest_preview_frame = None
                self._preview_sequence = 0
            threading.Thread(
                target=self._drain_stderr, args=(process, generation), daemon=True,
                name="video-capture-preview-stderr",
            ).start()
            threading.Thread(
                target=self._drain_preview_stdout, args=(process, generation), daemon=True,
                name="video-capture-preview-output",
            ).start()
            threading.Thread(
                target=self._watch_preview, args=(process, generation), daemon=True,
                name="video-capture-preview-watch",
            ).start()
            return self.status()

    def stop_preview(self) -> Dict[str, Any]:
        with self._operation_lock:
            with self._lock:
                if self._state in ("recording", "stopping"):
                    return self.status()
                process = self._process
                self._generation += 1
                self._reset_process_state()
                if self._state == "previewing":
                    self._state = "idle"
            if process is not None:
                self._terminate_process(process, graceful=False, timeout=3)
            return self.status()

    def open_preview_stream(self) -> Iterable[bytes]:
        with self._preview_condition:
            process = self._process
            generation = self._generation
            if process is None or process.poll() is not None or self._state not in (
                "previewing", "recording", "stopping",
            ):
                raise VideoCaptureBusyError("当前没有可用的视频预览")

        def generate() -> Iterable[bytes]:
            last_sequence = -1
            while True:
                with self._preview_condition:
                    self._preview_condition.wait_for(
                        lambda: generation != self._generation
                        or self._preview_sequence != last_sequence
                        or process.poll() is not None,
                        timeout=5,
                    )
                    if generation != self._generation:
                        break
                    if self._preview_sequence == last_sequence:
                        if process.poll() is not None:
                            break
                        continue
                    frame = self._latest_preview_frame
                    last_sequence = self._preview_sequence
                if frame:
                    yield (
                        b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                        + str(len(frame)).encode("ascii") + b"\r\n\r\n" + frame + b"\r\n"
                    )

        return generate()

    def _validate_finished_file(self, path: Path) -> bool:
        try:
            if path.stat().st_size <= 0:
                return False
        except OSError:
            return False
        if shutil.which(FFPROBE) is None:
            return True
        try:
            result = subprocess.run(
                [
                    FFPROBE, "-v", "error", "-select_streams", "v:0",
                    "-show_entries", "stream=codec_name", "-of", "json", str(path),
                ],
                capture_output=True, text=True, timeout=15,
            )
            streams = json.loads(result.stdout).get("streams", []) if result.returncode == 0 else []
            return bool(streams and streams[0].get("codec_name"))
        except (OSError, subprocess.TimeoutExpired, ValueError, TypeError, json.JSONDecodeError):
            return False

    def _finish_recording(self, process: subprocess.Popen, generation: int, return_code: int) -> None:
        with self._lock:
            if generation != self._generation or self._process is not process:
                return
            working_path = self._working_path
            output_path = self._output_path
            reason = self._stop_reason

        valid = bool(working_path and output_path and self._validate_finished_file(working_path))
        if valid and working_path and output_path:
            try:
                if output_path.exists():
                    raise FileExistsError(str(output_path))
                working_path.rename(output_path)
            except OSError as exc:
                valid = False
                with self._lock:
                    self._error = f"MP4 文件完成但重命名失败：{exc}"

        with self._lock:
            if generation != self._generation or self._process is not process:
                return
            self._reset_process_state()
            self._record_started_monotonic = None
            if valid:
                expected = reason in (
                    "manual", "size_limit", "storage_guard", "service_shutdown",
                )
                if return_code == 0 or expected:
                    self._state = "completed"
                    self._error = None
                else:
                    self._state = "error"
                    self._error = self._recent_pipeline_error("录像源异常中断，已保留可播放的 MP4")
            else:
                self._state = "error"
                if self._error is None:
                    self._error = self._recent_pipeline_error("录像没有生成可播放的 MP4 文件")
            self._record_done.set()

    def _request_record_stop(self, process: subprocess.Popen, generation: int, reason: str) -> None:
        with self._lock:
            if generation != self._generation or self._process is not process:
                return
            if self._state == "recording":
                self._state = "stopping"
                self._stop_reason = reason
            elif self._state != "stopping":
                return
        if process.poll() is None:
            try:
                process.send_signal(signal.SIGINT)
            except ProcessLookupError:
                pass

    def _monitor_recording(self, process: subprocess.Popen, generation: int) -> None:
        stop_requested_at: Optional[float] = None
        while process.poll() is None:
            with self._lock:
                if generation != self._generation or self._process is not process:
                    return
                state = self._state
                working_path = self._working_path
                save_directory = self._save_directory
                max_bytes = self._max_file_size_bytes

            if state == "recording" and working_path is not None:
                try:
                    size = working_path.stat().st_size
                except OSError:
                    size = 0
                stop_at = max(1, max_bytes - min(SIZE_STOP_MARGIN_BYTES, max_bytes // 10))
                if size >= stop_at:
                    self._request_record_stop(process, generation, "size_limit")
                    stop_requested_at = time.monotonic()
                elif save_directory is not None:
                    try:
                        disk = storage_snapshot(str(save_directory))
                        if int(disk["available_bytes"]) <= int(disk["reserve_bytes"]) + FINALIZE_MARGIN_BYTES:
                            self._request_record_stop(process, generation, "storage_guard")
                            stop_requested_at = time.monotonic()
                    except VideoCaptureError:
                        self._request_record_stop(process, generation, "storage_guard")
                        stop_requested_at = time.monotonic()
            elif state == "stopping" and stop_requested_at is None:
                stop_requested_at = time.monotonic()

            if stop_requested_at is not None and time.monotonic() - stop_requested_at > 15:
                self._terminate_process(process, graceful=False, timeout=2)
                break
            time.sleep(0.5)

        return_code = process.wait()
        with self._lock:
            if generation == self._generation and self._process is process and self._state == "recording":
                self._stop_reason = "source_ended"
        self._finish_recording(process, generation, return_code)

    def start_recording(
        self, source_input: Dict[str, Any], save_path: str, max_file_size_bytes: int,
    ) -> Dict[str, Any]:
        with self._operation_lock:
            with self._lock:
                if self._state in ("recording", "stopping"):
                    raise VideoCaptureBusyError("已有录像正在进行")

            initial_storage = storage_snapshot(save_path)
            validate_recording_capacity(initial_storage, max_file_size_bytes)
            directory = Path(initial_storage["path"])
            _write_probe(directory)

            self.stop_preview()
            source, probe = probe_source(source_input)

            latest_storage = storage_snapshot(str(directory))
            validate_recording_capacity(latest_storage, max_file_size_bytes)
            stem = _timestamp_stem()
            output_path = directory / f"{stem}.mp4"
            working_path = directory / f".{stem}.mp4.part"
            if output_path.exists() or working_path.exists():
                raise VideoCaptureRuntimeError("录像文件名冲突，请稍后重新开始")

            command = build_record_args(source, probe, working_path)
            process = self._spawn(command)
            now_ms = int(time.time() * 1000)
            with self._lock:
                self._generation += 1
                generation = self._generation
                self._process = process
                self._state = "recording"
                self._source = source
                self._probe = probe
                self._started_unix_ms = now_ms
                self._record_started_monotonic = time.monotonic()
                self._working_path = working_path
                self._output_path = output_path
                self._save_directory = directory
                self._max_file_size_bytes = max_file_size_bytes
                self._stop_reason = None
                self._error = None
                self._stderr.clear()
                self._latest_preview_frame = None
                self._preview_sequence = 0
                self._record_done = threading.Event()
            threading.Thread(
                target=self._drain_stderr, args=(process, generation), daemon=True,
                name="video-capture-record-stderr",
            ).start()
            threading.Thread(
                target=self._drain_preview_stdout, args=(process, generation), daemon=True,
                name="video-capture-record-output",
            ).start()
            threading.Thread(
                target=self._monitor_recording, args=(process, generation), daemon=True,
                name="video-capture-record-watch",
            ).start()
            return self.status()

    def stop_recording(self, reason: str = "manual") -> Dict[str, Any]:
        with self._operation_lock:
            with self._lock:
                process = self._process
                generation = self._generation
                done = self._record_done
                if process is None or self._state not in ("recording", "stopping"):
                    return self.status()
            self._request_record_stop(process, generation, reason)
            if not done.wait(timeout=22):
                self._terminate_process(process, graceful=False, timeout=2)
                done.wait(timeout=5)
            return self.status()

    def shutdown(self) -> None:
        with self._lock:
            recording = self._state in ("recording", "stopping")
        if recording:
            self.stop_recording("service_shutdown")
        else:
            self.stop_preview()


capture_manager = VideoCaptureManager()
