"""Native engine discovery and input validation. Python never performs inference."""
import copy
import fcntl
import json
import math
import os
from pathlib import Path
import re
import subprocess
import threading

from services.model_info import inspect_model_file

NPU_LOCK_FILE = Path('/run/lock/rk3588-visual-npu.lock')
MODEL_ID = re.compile(r'[a-z][a-z0-9_-]{0,63}\Z')
MAX_IMAGES = 100
MAX_MODELS = 8
MAX_IMAGE_BYTES = 20 * 1024 * 1024
MAX_UPLOAD_BYTES = 256 * 1024 * 1024
IMAGE_EXTENSIONS = {'.jpg', '.jpeg', '.png', '.bmp', '.webp'}


class JobError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


def npu_busy() -> bool:
    # Same inode and flock protocol as npu::DeviceOwner. Never unlink this file.
    try:
        with NPU_LOCK_FILE.open('a') as lock:
            try:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                return True
            fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
        return False
    except OSError as exc:
        raise JobError(503, f'无法检查 NPU 状态：{exc.strerror}') from exc


def engine_environment(runtime: Path) -> dict:
    env = os.environ.copy()
    env['LD_LIBRARY_PATH'] = str(runtime / 'libs') + os.pathsep + env.get('LD_LIBRARY_PATH', '')
    env['OPENCV_IO_MAX_IMAGE_PIXELS'] = '16777216'
    return env


class EngineCatalog:
    def __init__(self, runtime: Path):
        self.runtime = runtime.resolve()
        self.binary = self.runtime / 'image_inference_runner'
        self._cache = {}
        self._lock = threading.Lock()

    def capabilities(self) -> dict:
        binary = self.binary
        if not binary.is_file():
            raise JobError(503, '独立图片推理引擎尚未安装，请更新 Web 控制台。')
        stat = binary.stat()
        key = (str(binary), stat.st_ino, stat.st_size, stat.st_mtime_ns)
        with self._lock:
            if key not in self._cache:
                try:
                    # Older engines treat unknown flags as video configuration paths.
                    # Do not launch them just to discover that this protocol is absent.
                    marker = b'IMAGE_INFERENCE_CAPABILITIES='
                    supported, tail = False, b''
                    with binary.open('rb') as executable:
                        for chunk in iter(lambda: executable.read(1024 * 1024), b''):
                            if marker in tail + chunk:
                                supported = True
                                break
                            tail = chunk[-len(marker):]
                    if not supported:
                        raise ValueError('image CLI absent')
                    process = subprocess.run([str(binary), '--image-inference-capabilities'], cwd=self.runtime,
                        env=engine_environment(self.runtime), capture_output=True, text=True, errors='replace', timeout=10)
                    lines = [line.partition('=')[2] for line in process.stdout.splitlines()
                             if line.startswith('IMAGE_INFERENCE_CAPABILITIES=')]
                    data = json.loads(lines[-1]) if lines else None
                    if process.returncode or not isinstance(data, dict) or data.get('version') != 1:
                        raise ValueError('unsupported protocol')
                    models = data.get('models')
                    if (not isinstance(models, list) or not models or any(not isinstance(m, dict)
                            or not isinstance(m.get('type'), str) for m in models)):
                        raise ValueError('invalid model catalog')
                except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
                    raise JobError(503, '独立图片推理引擎不可用，请重新部署 Web 控制台。') from exc
                if len(self._cache) >= 32:
                    self._cache.clear()
                self._cache[key] = data
            data = copy.deepcopy(self._cache[key])
        data.update(max_images=MAX_IMAGES, max_models=MAX_MODELS, max_image_bytes=MAX_IMAGE_BYTES,
                    max_upload_bytes=MAX_UPLOAD_BYTES, busy=npu_busy())
        return data

    @staticmethod
    def asset(value: str, suffix: str) -> Path:
        if not isinstance(value, str) or not value or len(value) > 4096 or '\0' in value:
            raise JobError(422, '请选择模型和所需的标签文件，或输入板端绝对路径')
        path = Path(value)
        if not path.is_absolute() or path.suffix.lower() != suffix:
            raise JobError(422, f'请输入 {suffix} 文件的板端绝对路径：{value}')
        try:
            path = path.resolve(strict=True)
            if not path.is_file() or path.stat().st_size == 0:
                raise ValueError('不是有效的普通文件')
        except (OSError, ValueError, RuntimeError) as exc:
            raise JobError(422, f'无法读取模型或标签文件：{value}（{exc}）') from exc
        return path

    def inspect_model(self, path: str, model_type: str) -> dict:
        model = self.asset(path, '.rknn')
        if model_type not in {item['type'] for item in self.capabilities()['models']}:
            raise JobError(422, f'引擎不支持模型类型：{model_type}')
        try:
            return inspect_model_file(self.runtime, self.binary, model, model_type)
        except ValueError as exc:
            raise JobError(422, str(exc)) from exc

    def validate(self, requested: dict) -> dict:
        if not isinstance(requested, dict):
            raise JobError(422, '推理参数必须是对象')
        catalog = self.capabilities()
        supported = {item['type']: item for item in catalog['models']}
        models = requested.get('models')
        if not isinstance(models, list) or not 1 <= len(models) <= MAX_MODELS:
            raise JobError(422, f'每次请选择 1～{MAX_MODELS} 个模型')
        ids, validated_models = set(), []
        for supplied in models:
            if not isinstance(supplied, dict):
                raise JobError(422, '模型参数格式错误')
            model_id = supplied.get('id', '')
            if not isinstance(model_id, str) or not MODEL_ID.fullmatch(model_id) or model_id in ids:
                raise JobError(422, '模型 ID 必须唯一，以小写字母开头，仅包含小写字母、数字、下划线和短横线')
            ids.add(model_id)
            kind = supplied.get('model_type')
            if not isinstance(kind, str) or kind not in supported:
                raise JobError(422, f'引擎不支持模型类型：{kind}')
            model = dict(id=model_id, enable=True, model_type=kind)
            for field, default in [('obj_thresh', .3), ('nms_thresh', .45)]:
                value = supplied.get(field, default)
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not 0 <= value <= 1:
                    raise JobError(422, f'{model_id} 的 {field} 必须在 0～1 之间')
                model[field] = value
            core = supplied.get('npu_core', -1)
            if isinstance(core, bool) or not isinstance(core, int) or core not in (-1, 0, 1, 2):
                raise JobError(422, 'NPU 核心配置无效')
            model['npu_core'] = core
            classes = supplied.get('detect_classes', [])
            if not isinstance(classes, list) or len(classes) > 256 or any(not isinstance(c, str) or len(c) > 128 for c in classes):
                raise JobError(422, '检测类别必须是字符串列表')
            model['detect_classes'] = classes
            model_path = self.asset(supplied.get('model_path', ''), '.rknn')
            label = supplied.get('label_path', '')
            label_path = self.asset(label, '.txt') if label or supported[kind].get('requires_labels') else None
            model.update(model_path=str(model_path), label_path=str(label_path) if label_path else '')
            info = self.inspect_model(str(model_path), kind)
            if not info.get('ok'):
                raise JobError(422, f"{model_id}：{info.get('error') or '模型检查失败'}")
            validated_models.append(model)
        roi = requested.get('inference_roi', {})
        if not isinstance(roi, dict):
            raise JobError(422, '推理区域格式错误')
        mode = roi.get('mode', '')
        resize = roi.get('resize_mode', 'stretch')
        if mode not in ('', 'roi_only', 'full_plus_roi', 'full_frame_roi_filter') or resize not in ('stretch', 'letterbox', 'expand'):
            raise JobError(422, '推理区域模式无效')
        polygon = roi.get('polygon', []) if mode else []
        if not isinstance(polygon, list) or (mode and not 3 <= len(polygon) <= 64):
            raise JobError(422, '推理区域需要 3～64 个顶点')
        for point in polygon:
            if (not isinstance(point, list) or len(point) != 2 or any(isinstance(v, bool)
                    or not isinstance(v, (int, float)) or not math.isfinite(v) or not 0 <= v <= 1 for v in point)):
                raise JobError(422, '推理区域坐标必须在 0～1 之间')
        # Exact polygon geometry is validated by the shared native validator.
        roi = dict(mode=mode, resize_mode=resize, polygon=polygon)
        return dict(models=validated_models, inference_roi=roi)
