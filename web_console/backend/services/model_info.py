"""HTTP-service adapter for the C++ ModelInspection SDK.

Both canvas and image checks use the console-owned CLI. Python validates paths,
caches SDK verdicts and reads runtime status; it implements no tensor rules.
"""
import copy
import json
import os
import socket
import subprocess
import threading
import time
from collections import OrderedDict
from pathlib import Path
from typing import Any

from services import process_manager as pm
from services.app_paths import resolve_application_directory

_cache: OrderedDict[tuple, tuple[float, dict]] = OrderedDict()
_probe_lock = threading.Lock()
_MAX_CACHE = 128
_ERROR_TTL = 30
MODEL_TYPES = {"yolov8_obb", "yolov5", "yolov8_det", "yolov8_pose", "yolo26_pose", "yolo26_depth", "zip_depth", "yolov5_seg"}


def app_directory(root: Path, name: str) -> Path:
    try:
        return resolve_application_directory(root, name)
    except FileNotFoundError as exc:
        raise ValueError("程序不存在或路径非法") from exc


def _model_path(app: Path, path: str) -> Path:
    relative = Path(path)
    target = (app / relative).resolve()
    assets = (app / "assets").resolve()
    if (relative.is_absolute() or ".." in relative.parts or assets.parent != app.resolve()
            or target.parent != assets or target.suffix.lower() != ".rknn"):
        raise ValueError("模型文件必须位于程序 assets/ 目录内，且为 .rknn 文件")
    if not target.is_file():
        raise ValueError(f"模型文件不存在: {path}")
    return target


def _fingerprint(path: Path) -> tuple:
    stat = path.stat()
    return (str(path), stat.st_ino, stat.st_size, stat.st_mtime_ns, stat.st_ctime_ns)


def _version(path: Path) -> str:
    stat = path.stat()
    seconds, nanos = divmod(stat.st_mtime_ns, 1_000_000_000)
    return f"{stat.st_ino}:{stat.st_size}:{seconds}:{nanos}"


def _probe(app: Path, binary: Path, model: Path) -> dict:
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(app / "libs") + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    try:
        result = subprocess.run(
            [str(binary), "--inspect-model", str(model)], cwd=app, env=env,
            capture_output=True, text=True, errors="replace", timeout=20,
        )
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "模型检查超时，请检查 RKNN 模型和 NPU 可用状态"}
    except OSError as exc:
        return {"ok": False, "error": f"无法启动模型检查: {exc}"}
    lines = [line[len("MODEL_INFO_JSON="):] for line in result.stdout.splitlines()
             if line.startswith("MODEL_INFO_JSON=")]
    if not lines:
        return {"ok": False, "error": "引擎未返回模型信息，请更新支持 --inspect-model 的程序包，或检查引擎运行库"}
    try:
        info = json.loads(lines[-1])
        if not isinstance(info, dict) or not isinstance(info.get("ok"), bool):
            raise ValueError("invalid response")
        if result.returncode != 0 and info["ok"]:
            raise ValueError("probe process failed")
        if info["ok"] and (info.get("inspection_version") != 1 or not isinstance(info.get("compatibility"), dict)):
            return {"ok": False, "error": "模型检查引擎版本不兼容，请更新 Web 控制台"}
        return info
    except (ValueError, TypeError):
        return {"ok": False, "error": "引擎返回的模型信息无效"}


def compatibility_error(info: dict, model_type: str) -> str:
    """Forward the SDK verdict; decoder rules live only in C++."""
    if model_type not in MODEL_TYPES:
        return f"不支持的模型类型: {model_type}"
    verdicts = info.get("compatibility")
    verdict = verdicts.get(model_type) if isinstance(verdicts, dict) else None
    if (info.get("inspection_version") != 1 or not isinstance(verdict, dict)
            or not isinstance(verdict.get("ok"), bool)):
        return "模型检查引擎版本不兼容，请更新 Web 控制台"
    if verdict["ok"]:
        return ""
    return verdict.get("error") if isinstance(verdict.get("error"), str) and verdict["error"] else "模型与所选类型不兼容"


def inspection_runtime() -> Path:
    root = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps")) / "_console"
    return Path(os.environ.get("IMAGE_INFERENCE_RUNTIME", str(root / "inference_runtime"))).resolve()


def inspect_model(app: Path, path: str, model_type: str = "") -> dict:
    model = _model_path(app, path)
    runtime = inspection_runtime()
    binary = runtime / "image_inference_runner"
    if not binary.is_file():
        raise ValueError("独立模型检查引擎尚未安装，请更新 Web 控制台")
    info = inspect_model_file(runtime, binary, model, model_type)
    info["path"] = path
    return info


def inspect_model_file(runtime: Path, binary: Path, model: Path, model_type: str = "") -> dict:
    """Shared probe/cache/decoder checks; callers validate their own file locations."""
    # Serializing probes also bounds the extra NPU memory use to one context.
    with _probe_lock:
        key = (_fingerprint(binary), _fingerprint(model))
        cached = _cache.get(key)
        if cached and (cached[1].get("ok") or time.monotonic() - cached[0] < _ERROR_TTL):
            _cache.move_to_end(key)
            info = copy.deepcopy(cached[1])
        else:
            info = _probe(runtime, binary, model)
            # Refuse to cache metadata for a file replaced during the probe.
            if key != (_fingerprint(binary), _fingerprint(model)):
                raise ValueError("检查期间模型或引擎被替换，请重新检查")
            _cache[key] = (time.monotonic(), copy.deepcopy(info))
            while len(_cache) > _MAX_CACHE:
                _cache.popitem(last=False)
        info["path"] = str(model)
        info["file_version"] = _version(model)
    if info.get("ok") and model_type:
        error = compatibility_error(info, model_type)
        if error:
            info.update(ok=False, error=error)
    return info


def validate_model_config(app: Path, config: dict[str, Any]) -> None:
    channels = config.get("channels", [])
    if not isinstance(channels, list):
        raise ValueError("channels 必须为数组")
    for index, channel in enumerate(channels):
        if not isinstance(channel, dict):
            raise ValueError("通道配置必须为对象")
        models = channel.get("models", [])
        if not isinstance(models, list):
            raise ValueError("通道 models 必须为数组")
        for model in models:
            if not isinstance(model, dict):
                raise ValueError("模型配置必须为对象")
            if not model.get("enable", True):
                continue
            label = f"通道 {channel.get('id', index)} / 模型 {model.get('id', '')}"
            try:
                model_type = model.get("model_type")
                if not isinstance(model_type, str) or model_type not in MODEL_TYPES:
                    raise ValueError(f"启用的模型必须指定有效 model_type: {model_type}")
                info = inspect_model(app, str(model.get("model_path", "")),
                                     model_type)
            except (ValueError, OSError) as exc:
                raise ValueError(f"{label}: {exc}") from exc
            if not info.get("ok"):
                raise ValueError(f"{label}: {info.get('error', '模型检查失败')}")
            # Every child gets its own input resize and result coordinate mapping.
            # Compatibility is checked per model; siblings need not share a size.


def runtime_status(app: Path, name: str) -> dict:
    process = pm.get_status(name)
    response = {"status": process["status"], "config": process.get("config"), "channels": []}
    if process["status"] != "running":
        return response
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(1.5)
            client.connect(str(app / "run.control.sock"))
            client.sendall(b'{"scope":"models","action":"status"}\n')
            chunks = bytearray()
            while len(chunks) < 1024 * 1024:
                chunk = client.recv(65536)
                if not chunk:
                    break
                chunks.extend(chunk)
                if b"\n" in chunk:
                    break
        data = json.loads(chunks.decode("utf-8"))
        if not data.get("ok") or not isinstance(data.get("channels"), list):
            raise ValueError("引擎不支持模型状态查询，请更新程序包")
        response.update(data)
    except (OSError, ValueError) as exc:
        response["error"] = f"无法获取实际运行模型: {exc}"
    return response
