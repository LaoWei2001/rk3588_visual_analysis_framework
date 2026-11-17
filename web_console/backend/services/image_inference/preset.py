"""Optional canvas adapter; the standalone task API only accepts file paths."""
import copy
from pathlib import Path

from services.app_paths import resolve_application_directory
from .engine import EngineCatalog, JobError, MAX_MODELS


def resolve_canvas_preset(apps_root: Path, name: str, settings: dict) -> dict:
    try:
        app = resolve_application_directory(apps_root, name)
    except (ValueError, FileNotFoundError) as exc:
        raise JobError(422, f'无法读取画布来源：{exc}') from exc
    models = settings.get('models')
    if not isinstance(models, list) or not 1 <= len(models) <= MAX_MODELS:
        raise JobError(422, f'画布预设需要 1～{MAX_MODELS} 个模型')
    result = copy.deepcopy(settings)
    for model in result['models']:
        if not isinstance(model, dict):
            raise JobError(422, '模型参数格式错误')
        for field, suffix in [('model_path', '.rknn'), ('label_path', '.txt')]:
            value = model.get(field, '')
            if not value:
                continue
            if not isinstance(value, str):
                raise JobError(422, '模型路径必须是字符串')
            path = Path(value)
            if not path.is_absolute():
                path = (app / path).resolve()
                assets = (app / 'assets').resolve()
                if assets.parent != app or path.parent != assets:
                    raise JobError(422, '画布相对路径必须位于来源程序 assets 目录内')
            model[field] = str(EngineCatalog.asset(str(path), suffix))
    return result
