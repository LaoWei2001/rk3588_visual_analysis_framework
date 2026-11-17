"""Export ordinary YOLO detection boxes from the native engine's result data."""
import math

from .engine import JobError

DETECTION_TYPES = frozenset(('yolov5', 'yolov8_det'))


def detection_model(settings: dict) -> dict:
    models = settings.get('models', [])
    if len(models) != 1 or models[0].get('model_type') not in DETECTION_TYPES:
        raise JobError(422, '仅支持使用单个 YOLO 目标检测模型的结果导出标签')
    return models[0]


def yolo_labels(result: dict, model: dict) -> str:
    """Boxes already cover the full image, including restored ROI coordinates.

    Normalize using coordinate_width/height, not the source or model input size.
    Successful images with no detections deliberately produce an empty TXT.
    """
    try:
        models = result['models']
        if (len(models) != 1 or models[0]['id'] != model['id']
                or models[0]['type'] != model['model_type'] or models[0]['succeeded'] is not True):
            raise ValueError('模型结果不匹配或推理失败')

        def number(value):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                raise ValueError('坐标或类别不是有效数字')
            return value

        width, height = number(result['coordinate_width']), number(result['coordinate_height'])
        if width <= 0 or height <= 0:
            raise ValueError('坐标尺寸无效')
        lines = []
        for obj in result['results']:
            if obj['model_id'] != model['id'] or obj['model_type'] != model['model_type']:
                raise ValueError('目标所属模型不匹配')
            class_id = number(obj['class_id'])
            if class_id < 0 or int(class_id) != class_id:
                raise ValueError('类别编号无效')
            box = obj['box']
            x, y, w, h = (number(box[key]) for key in ('x', 'y', 'width', 'height'))
            if w <= 0 or h <= 0:
                raise ValueError('检测框尺寸无效')
            left, top = max(0, min(width, x)), max(0, min(height, y))
            right, bottom = max(0, min(width, x + w)), max(0, min(height, y + h))
            if right <= left or bottom <= top:
                continue
            values = ((left + right) / (2 * width), (top + bottom) / (2 * height),
                      (right - left) / width, (bottom - top) / height)
            lines.append(f'{int(class_id)} ' + ' '.join(f'{value:.8f}' for value in values))
        return ''.join(line + '\n' for line in lines)
    except (KeyError, TypeError, ValueError, OverflowError) as exc:
        raise JobError(422, f'结果无法导出 YOLO 标签：{exc}') from exc
