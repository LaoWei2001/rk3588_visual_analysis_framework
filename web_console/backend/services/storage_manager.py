"""设备存储状态与清理策略。

自动清理严格限制在 ``APPS_ROOT/.data/*/event_store/<event_id>``。此外提供一组
需要用户手动确认的 root 开发工具缓存白名单；接口只接收白名单标识，不接收路径。
"""
from __future__ import annotations

import json
import os
import pwd
import shutil
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

from services.data_dir import APPS_ROOT


SETTINGS_FILE = Path(os.environ.get(
    "STORAGE_SETTINGS_FILE", str(APPS_ROOT / ".console_storage_settings.json")
))
try:
    _ROOT_HOME_DEFAULT = pwd.getpwnam("root").pw_dir
except KeyError:
    _ROOT_HOME_DEFAULT = "/root"
ROOT_HOME = Path(os.environ.get("ROOT_HOME", _ROOT_HOME_DEFAULT))

# 只允许清理 root 主目录下这些直接子项。Node 运行环境 .nvm 有意不在白名单中。
ROOT_CLEANUP_TARGETS: Dict[str, Dict[str, str]] = {
    "vscode_server": {
        "name": ".vscode-server",
        "label": "VS Code Server 历史文件",
        "description": "远程 VS Code Server 版本、扩展及运行缓存；再次连接时会按需重装。",
    },
    "vscode": {
        "name": ".vscode",
        "label": "VS Code 配置残留",
        "description": "root 用户下的 VS Code 配置和扩展残留。",
    },
    "vscode_root": {
        "name": ".vscode-root",
        "label": "VS Code Root 残留",
        "description": "部分远程开发环境生成的 root 专用残留目录。",
    },
    "claude": {
        "name": ".claude",
        "label": "Claude 缓存与配置",
        "description": "root 用户下的 Claude 工具缓存与本地配置。",
    },
    "cache": {
        "name": ".cache",
        "label": "通用软件缓存",
        "description": "包括 pip 等软件的下载与构建缓存；后续使用时可能重新下载。",
    },
    "npm": {
        "name": ".npm",
        "label": "npm 包缓存",
        "description": "npm 下载缓存和日志；不会删除 .nvm 中的 Node 运行环境。",
    },
}
DEFAULTS: Dict[str, Any] = {
    "auto_cleanup": False,
    "retention_days": 30,
    "max_event_store_gb": 1.0,
    "min_free_gb": 1.0,
}

_settings_lock = threading.RLock()
_cleanup_lock = threading.Lock()
_root_cleanup_lock = threading.Lock()
_last_cleanup: Optional[Dict[str, Any]] = None


def _number(value: Any, default: float, minimum: float, maximum: float) -> float:
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return default
    return min(maximum, max(minimum, parsed))


def read_settings() -> Dict[str, Any]:
    try:
        raw = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
    except (OSError, ValueError, TypeError):
        raw = {}
    return {
        "auto_cleanup": bool(raw.get("auto_cleanup", DEFAULTS["auto_cleanup"])),
        "retention_days": int(_number(raw.get("retention_days"), 30, 1, 3650)),
        "max_event_store_gb": _number(raw.get("max_event_store_gb"), 1.0, 0.1, 1000.0),
        "min_free_gb": _number(raw.get("min_free_gb"), 1.0, 0.0, 1000.0),
    }


def write_settings(settings: Dict[str, Any]) -> Dict[str, Any]:
    normalized = {
        "auto_cleanup": bool(settings.get("auto_cleanup", False)),
        "retention_days": int(_number(settings.get("retention_days"), 30, 1, 3650)),
        "max_event_store_gb": _number(settings.get("max_event_store_gb"), 1.0, 0.1, 1000.0),
        "min_free_gb": _number(settings.get("min_free_gb"), 1.0, 0.0, 1000.0),
    }
    with _settings_lock:
        SETTINGS_FILE.parent.mkdir(parents=True, exist_ok=True)
        temporary = SETTINGS_FILE.with_suffix(SETTINGS_FILE.suffix + ".tmp")
        temporary.write_text(
            json.dumps({"version": 1, **normalized}, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, SETTINGS_FILE)
    return normalized


def _existing_fs_path() -> Path:
    path = APPS_ROOT
    while not path.exists() and path != path.parent:
        path = path.parent
    return path


def _event_stores() -> List[Path]:
    root = APPS_ROOT / ".data"
    if not root.is_dir():
        return []
    stores: List[Path] = []
    try:
        app_dirs = list(root.iterdir())
    except OSError:
        return []
    for app_dir in app_dirs:
        store = app_dir / "event_store"
        if app_dir.is_dir() and not app_dir.is_symlink() and store.is_dir() and not store.is_symlink():
            stores.append(store)
    return stores


def _tree_bytes(path: Path) -> int:
    total = 0
    try:
        for root, dirs, files in os.walk(path, followlinks=False):
            dirs[:] = [name for name in dirs if not (Path(root) / name).is_symlink()]
            for name in files:
                item = Path(root) / name
                if not item.is_symlink():
                    try:
                        total += item.stat().st_size
                    except OSError:
                        pass
    except OSError:
        pass
    return total


def _path_exists(path: Path) -> bool:
    """Path.exists() 对断开的符号链接返回 False，这里仍要把链接列为可清理。"""
    return os.path.lexists(path)


def _path_bytes(path: Path) -> int:
    if path.is_symlink():
        try:
            return path.lstat().st_size
        except OSError:
            return 0
    if path.is_dir():
        return _tree_bytes(path)
    try:
        return path.stat().st_size
    except OSError:
        return 0


def _root_cleanup_path(name: str) -> Path:
    """构造 root 主目录直接子项，不解析符号链接指向。"""
    root_home = ROOT_HOME.absolute()
    target = root_home / name
    if Path(name).name != name or target.parent != root_home:
        raise ValueError("root 清理目标配置无效")
    return target


def root_cleanup_snapshot() -> List[Dict[str, Any]]:
    targets: List[Dict[str, Any]] = []
    for key, config in ROOT_CLEANUP_TARGETS.items():
        path = _root_cleanup_path(config["name"])
        exists = _path_exists(path)
        targets.append({
            "key": key,
            "label": config["label"],
            "description": config["description"],
            "path": str(path),
            "exists": exists,
            "bytes": _path_bytes(path) if exists else 0,
        })
    return targets


def cleanup_root_targets(keys: List[str]) -> Dict[str, Any]:
    """删除用户明确选择的白名单项；所有 key 验证通过后才开始删除。"""
    requested = list(dict.fromkeys(keys))
    if not requested:
        raise ValueError("请至少选择一个清理项")
    invalid = [key for key in requested if key not in ROOT_CLEANUP_TARGETS]
    if invalid:
        raise ValueError(f"不允许清理这些项目：{', '.join(invalid)}")

    deleted: List[Dict[str, Any]] = []
    errors: List[Dict[str, str]] = []
    freed_bytes = 0
    with _root_cleanup_lock:
        for key in requested:
            config = ROOT_CLEANUP_TARGETS[key]
            path = _root_cleanup_path(config["name"])
            if not _path_exists(path):
                continue
            size = _path_bytes(path)
            try:
                # rmtree 不跟随目录符号链接；显式 unlink 也只删除链接本身。
                if path.is_symlink() or not path.is_dir():
                    path.unlink()
                else:
                    shutil.rmtree(path)
            except OSError as exc:
                errors.append({"key": key, "path": str(path), "error": str(exc)})
                continue
            deleted.append({"key": key, "path": str(path), "bytes": size})
            freed_bytes += size
    return {
        "finished_unix_ms": int(time.time() * 1000),
        "deleted_count": len(deleted),
        "freed_bytes": freed_bytes,
        "deleted": deleted,
        "errors": errors,
    }


def _json(path: Path) -> Dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
        return value if isinstance(value, dict) else {}
    except (OSError, ValueError, TypeError):
        return {}


def _event_info(store: Path, path: Path) -> Optional[Dict[str, Any]]:
    """读取一个可管理事件。解析失败时保守跳过，绝不把未知目录当作可删除事件。"""
    if not path.is_dir() or path.is_symlink() or path.parent != store:
        return None
    event_file = path / "event.json"
    media_file = path / "media_state.json"
    delivery_file = path / "delivery_state.json"
    if not all(item.is_file() and not item.is_symlink() for item in (event_file, media_file, delivery_file)):
        return None

    event_doc = _json(event_file)
    media_doc = _json(media_file)
    delivery_doc = _json(delivery_file)
    if not event_doc or not media_doc or not delivery_doc:
        return None

    active = False
    media = media_doc.get("media", {})
    if isinstance(media, dict):
        for value in media.values():
            if isinstance(value, dict) and value.get("status") in ("requested", "generating"):
                active = True
                break
    deliveries = delivery_doc.get("deliveries", [])
    if isinstance(deliveries, list) and any(
        isinstance(value, dict) and value.get("status") == "uploading" for value in deliveries
    ):
        active = True

    event = event_doc.get("event", {})
    created = event.get("created_unix_sec", 0) if isinstance(event, dict) else 0
    try:
        created_at = float(created) if float(created) > 0 else event_file.stat().st_mtime
    except (OSError, TypeError, ValueError):
        return None
    return {
        "path": path,
        "store": store,
        "app": store.parent.name,
        "created_at": created_at,
        "bytes": _tree_bytes(path),
        "active": active,
    }


def _events() -> List[Dict[str, Any]]:
    events: List[Dict[str, Any]] = []
    for store in _event_stores():
        try:
            children = list(store.iterdir())
        except OSError:
            continue
        for child in children:
            value = _event_info(store, child)
            if value is not None:
                events.append(value)
    return events


def _remove_event(event: Dict[str, Any]) -> bool:
    path = event["path"]
    store = event["store"]
    try:
        # 删除前再次验证直接父目录和文件标记，避免扫描后路径被替换。
        if path.parent != store or path.is_symlink() or not (path / "event.json").is_file():
            return False
        shutil.rmtree(path)
        return True
    except OSError:
        return False


def storage_snapshot() -> Dict[str, Any]:
    usage = shutil.disk_usage(_existing_fs_path())
    events = _events()
    settings = read_settings()
    # 与 df 的 Use% 口径一致：root 保留块不属于普通服务可用空间，分母用 used+avail。
    usable_total = usage.used + usage.free
    used_percent = (usage.used * 100.0 / usable_total) if usable_total else 0.0
    return {
        **settings,
        "storage_path": str(APPS_ROOT),
        "total_bytes": usage.total,
        "used_bytes": usage.used,
        "free_bytes": usage.free,
        "used_percent": round(used_percent, 1),
        "event_bytes": sum(value["bytes"] for value in events),
        "event_count": len(events),
        "root_cleanup_targets": root_cleanup_snapshot(),
        "last_cleanup": _last_cleanup,
    }


def cleanup_now() -> Dict[str, Any]:
    global _last_cleanup
    with _cleanup_lock:
        settings = read_settings()
        events = _events()
        deleted_count = 0
        deleted_bytes = 0
        skipped_active = sum(1 for event in events if event["active"])

        def remove(event: Dict[str, Any]) -> bool:
            nonlocal deleted_count, deleted_bytes
            if event["active"] or event.get("deleted"):
                return False
            if not _remove_event(event):
                return False
            event["deleted"] = True
            deleted_count += 1
            deleted_bytes += event["bytes"]
            return True

        cutoff = time.time() - settings["retention_days"] * 86400
        for event in sorted(events, key=lambda value: value["created_at"]):
            if event["created_at"] < cutoff:
                remove(event)

        max_bytes = int(settings["max_event_store_gb"] * 1024 ** 3)
        by_store: Dict[Path, List[Dict[str, Any]]] = {}
        for event in events:
            if not event.get("deleted"):
                by_store.setdefault(event["store"], []).append(event)
        for store_events in by_store.values():
            current = sum(event["bytes"] for event in store_events)
            for event in sorted(store_events, key=lambda value: value["created_at"]):
                if current <= max_bytes:
                    break
                if remove(event):
                    current -= event["bytes"]

        min_free_bytes = int(settings["min_free_gb"] * 1024 ** 3)
        current_free = shutil.disk_usage(_existing_fs_path()).free
        for event in sorted(events, key=lambda value: value["created_at"]):
            if current_free >= min_free_bytes:
                break
            if remove(event):
                current_free += event["bytes"]

        _last_cleanup = {
            "finished_unix_ms": int(time.time() * 1000),
            "deleted_count": deleted_count,
            "deleted_bytes": deleted_bytes,
            "skipped_active": skipped_active,
        }
        return dict(_last_cleanup)


def vision_environment() -> Dict[str, str]:
    settings = read_settings()
    return {
        "EVENT_STORE_MAX_BYTES": str(int(settings["max_event_store_gb"] * 1024 ** 3)),
        "EVENT_STORE_MIN_FREE_BYTES": str(int(settings["min_free_gb"] * 1024 ** 3)),
    }
