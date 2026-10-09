"""RKNN metadata probes and cached, save-time compatibility checks.

Only probes create an RKNN context; runtime status reads the engine's lifecycle snapshot.
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

_cache: OrderedDict[tuple, tuple[float, dict]] = OrderedDict()
_probe_lock = threading.Lock()
_MAX_CACHE = 128
_ERROR_TTL = 30
MODEL_TYPES = {"yolov5", "yolov8_det", "yolov8_pose", "yolo26_pose", "yolov5_seg"}


def app_directory(root: Path, name: str) -> Path:
    if not name or Path(name).name != name or name in {".", ".."}:
        raise ValueError("非法程序名称")
    app = (root / name).resolve()
    if app.parent != root.resolve() or not app.is_dir():
        raise ValueError("程序不存在或路径非法")
    return app


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
        return info
    except (ValueError, TypeError):
        return {"ok": False, "error": "引擎返回的模型信息无效"}


def compatibility_error(info: dict, model_type: str) -> str:
    """Check the tensor contracts used by the existing decoders; never infer type from filename."""
    if model_type not in MODEL_TYPES:
        return f"不支持的模型类型: {model_type}"
    if model_type in {"yolov5", "yolov5_seg"} and info.get("format") != "NHWC":
        return f"{model_type} 当前要求 NHWC 输入布局"
    outputs = info.get("outputs", [])
    width, height = info.get("width", 0), info.get("height", 0)
    if model_type == "yolo26_pose":
        shape = [dim for dim in outputs[0].get("shape", []) if dim != 1] if len(outputs) == 1 else []
        if len(shape) != 2 or 56 not in shape or min(shape) <= 0:
            return "yolo26_pose 要求单输出，特征维为 56（17 个关键点）"
        if outputs[0].get("elements") != shape[0] * shape[1]:
            return "yolo26_pose 输出元素数量与张量形状不匹配"
        if outputs[0].get("type") not in {0, 1, 2, 3}:
            return "yolo26_pose 输出仅支持 FLOAT32/FLOAT16/INT8/UINT8"
        if outputs[0]["type"] in {2, 3} and outputs[0].get("scale", 0) > 1:
            return "yolo26_pose INT8 输出量化 scale 大于 1，无法保留置信度，需要重新导出模型"
        return ""
    if width <= 0 or height <= 0 or width % 32 or height % 32:
        return f"{model_type} 当前要求输入宽高为 32 的整数倍"
    if model_type == "yolov8_det":
        if len(outputs) != 9:
            return "yolov8_det 要求三个检测分支、共 9 个输出张量"
        for i, stride in enumerate((8, 16, 32)):
            box, score = outputs[i * 3:i * 3 + 2]
            shape = box.get("shape", [])
            score_shape = score.get("shape", [])
            if (box.get("format") != "NCHW" or score.get("format") != "NCHW"
                    or len(shape) != 4 or len(score_shape) != 4 or shape[0] != 1
                    or shape[1] < 4 or shape[1] % 4 or score_shape[1] <= 0
                    or shape[2:] != [height // stride, width // stride]
                    or score_shape[0] != 1 or score_shape[2:] != shape[2:]):
                return "yolov8_det 输出布局或检测网格与当前解码器不兼容"
    elif model_type in {"yolov5", "yolov5_seg"}:
        count = 3 if model_type == "yolov5" else 7
        if len(outputs) != count:
            return f"{model_type} 当前要求 {count} 个输出张量"
        for i, stride in enumerate((8, 16, 32)):
            index = i if count == 3 else i * 2
            head = outputs[index]
            shape = head.get("shape", [])
            if (head.get("format") != "NCHW" or len(shape) != 4 or shape[0] != 1
                    or shape[1] <= 15 or shape[1] % 3
                    or shape[2:] != [height // stride, width // stride]):
                return f"{model_type} 检测头输出布局与当前解码器不兼容"
            if count == 7:
                coeff = outputs[index + 1]
                if coeff.get("format") != "NCHW" or coeff.get("shape") != [1, 96, *shape[2:]]:
                    return "yolov5_seg 要求每个检测头对应 32 维 mask 系数"
        if count == 7 and (outputs[6].get("format") != "NCHW"
                           or outputs[6].get("shape") != [1, 32, 160, 160]):
            return "yolov5_seg 当前仅支持 32×160×160 的 proto 输出，其他尺寸需先升级分割解码器"
    elif model_type == "yolov8_pose":
        if len(outputs) < 4:
            return "yolov8_pose 要求三个检测头和关键点输出"
        candidates = 0
        for head in outputs[:3]:
            shape = head.get("shape", [])
            if (head.get("format") != "NCHW" or len(shape) != 4
                    or shape[0] != 1 or shape[1] != 65 or min(shape[2:]) <= 0):
                return "yolov8_pose 当前要求 NCHW 的 65 通道检测头"
            candidates += shape[2] * shape[3]
        if not any(out.get("elements", 0) > 0 and out["elements"] % (candidates * 3) == 0
                   for out in outputs[3:]):
            return "yolov8_pose 关键点输出数量与检测候选数不匹配"
    return ""


def inspect_model(app: Path, path: str, model_type: str = "") -> dict:
    model = _model_path(app, path)
    binary = app / pm.BINARY_NAME
    if not binary.is_file():
        raise ValueError("程序包缺少推理引擎，无法检查模型")
    # Serializing probes also bounds the extra NPU memory use to one context.
    with _probe_lock:
        key = (_fingerprint(binary), _fingerprint(model))
        cached = _cache.get(key)
        if cached and (cached[1].get("ok") or time.monotonic() - cached[0] < _ERROR_TTL):
            _cache.move_to_end(key)
            info = copy.deepcopy(cached[1])
        else:
            info = _probe(app, binary, model)
            # Refuse to cache metadata for a file replaced during the probe.
            if key != (_fingerprint(binary), _fingerprint(model)):
                raise ValueError("检查期间模型或引擎被替换，请重新检查")
            _cache[key] = (time.monotonic(), copy.deepcopy(info))
            while len(_cache) > _MAX_CACHE:
                _cache.popitem(last=False)
        info["path"] = path
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
        if not channel.get("enable", True) or not channel.get("infer_enable", True):
            continue
        expected = None
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
            size = (info["width"], info["height"])
            if expected is not None and size != expected:
                raise ValueError(f"{label}: 同通道多个模型输入尺寸必须一致，已选 {expected[0]}×{expected[1]}，该模型为 {size[0]}×{size[1]}")
            expected = size


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
