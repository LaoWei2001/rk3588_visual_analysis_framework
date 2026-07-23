"""Web 控制台运行意图的持久化状态。

这里保存的是用户意图，不是瞬时 PID/systemd 状态：
  - autostart: 用户是否勾选“开机自启”；
  - desired_running: 用户最后一次操作是启动还是停止。

只有两者同时为 true，控制台在下次启动时才恢复对应组件。状态文件放在
APPS_ROOT 的隐藏文件中，不属于任何一个 App，升级/删除 App 时不会误覆盖。
"""
from __future__ import annotations

import fcntl
import json
import os
import threading
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Callable, Dict, Iterator, Optional

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
STATE_FILE = Path(os.environ.get(
    "RUNTIME_STATE_FILE", str(APPS_ROOT / ".console_runtime_state.json")
))
LOCK_FILE = STATE_FILE.with_suffix(STATE_FILE.suffix + ".lock")

_thread_lock = threading.RLock()
_SERVICE_KEYS = ("ota_agent", "unified_upload")


def _default_state() -> Dict[str, Any]:
    return {
        "version": 1,
        "vision": {"last_app": None, "apps": {}},
        "services": {
            key: {"autostart": False, "desired_running": False}
            for key in _SERVICE_KEYS
        },
    }


def _normalize(raw: Any) -> Dict[str, Any]:
    state = _default_state()
    if not isinstance(raw, dict):
        return state

    vision = raw.get("vision")
    if isinstance(vision, dict):
        last_app = vision.get("last_app")
        state["vision"]["last_app"] = last_app if isinstance(last_app, str) and last_app else None
        apps = vision.get("apps")
        if isinstance(apps, dict):
            for name, value in apps.items():
                if not isinstance(name, str) or not name or not isinstance(value, dict):
                    continue
                mode = value.get("mode") if value.get("mode") in ("deploy", "debug") else "deploy"
                config = value.get("config")
                if not isinstance(config, str) or not config:
                    config = "config.json"
                state["vision"]["apps"][name] = {
                    "autostart": bool(value.get("autostart", False)),
                    "desired_running": bool(value.get("desired_running", False)),
                    "mode": mode,
                    "config": config,
                }

    services = raw.get("services")
    if isinstance(services, dict):
        for key in _SERVICE_KEYS:
            value = services.get(key)
            if isinstance(value, dict):
                state["services"][key] = {
                    "autostart": bool(value.get("autostart", False)),
                    "desired_running": bool(value.get("desired_running", False)),
                }
    return state


@contextmanager
def _locked() -> Iterator[None]:
    STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    with _thread_lock:
        with LOCK_FILE.open("a+") as lock:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def _read_unlocked() -> Dict[str, Any]:
    try:
        return _normalize(json.loads(STATE_FILE.read_text(encoding="utf-8")))
    except (OSError, ValueError, TypeError):
        return _default_state()


def _write_unlocked(state: Dict[str, Any]) -> None:
    temporary = STATE_FILE.with_suffix(STATE_FILE.suffix + ".tmp")
    temporary.write_text(
        json.dumps(_normalize(state), ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    os.replace(temporary, STATE_FILE)


def _update(mutator: Callable[[Dict[str, Any]], None]) -> Dict[str, Any]:
    with _locked():
        state = _read_unlocked()
        mutator(state)
        _write_unlocked(state)
        return state


def get_vision_settings(app_name: str) -> Dict[str, Any]:
    with _locked():
        state = _read_unlocked()
    value = state["vision"]["apps"].get(app_name, {})
    return {
        "autostart": bool(value.get("autostart", False)),
        "desired_running": bool(value.get("desired_running", False)),
        "mode": value.get("mode", "deploy"),
        "config": value.get("config", "config.json"),
    }


def set_vision_autostart(app_name: str, enabled: bool) -> Dict[str, Any]:
    def change(state: Dict[str, Any]) -> None:
        apps = state["vision"]["apps"]
        current = apps.setdefault(app_name, {
            "autostart": False,
            "desired_running": False,
            "mode": "deploy",
            "config": "config.json",
        })
        current["autostart"] = bool(enabled)

    state = _update(change)
    return dict(state["vision"]["apps"][app_name])


def mark_vision_started(app_name: str, mode: str, config: str) -> None:
    def change(state: Dict[str, Any]) -> None:
        apps = state["vision"]["apps"]
        # 视觉程序全局单实例，因此最多只能有一个“最后要求运行”的 App。
        for value in apps.values():
            value["desired_running"] = False
        current = apps.setdefault(app_name, {
            "autostart": False,
            "desired_running": False,
            "mode": "deploy",
            "config": "config.json",
        })
        current.update({
            "desired_running": True,
            "mode": mode if mode in ("deploy", "debug") else "deploy",
            "config": config or "config.json",
        })
        state["vision"]["last_app"] = app_name

    _update(change)


def mark_vision_stopped(app_name: str) -> None:
    def change(state: Dict[str, Any]) -> None:
        current = state["vision"]["apps"].get(app_name)
        if current is not None:
            current["desired_running"] = False

    _update(change)


def remove_vision_app(app_name: str) -> None:
    def change(state: Dict[str, Any]) -> None:
        state["vision"]["apps"].pop(app_name, None)
        if state["vision"].get("last_app") == app_name:
            state["vision"]["last_app"] = None

    _update(change)


def get_vision_boot_target() -> Optional[Dict[str, str]]:
    with _locked():
        state = _read_unlocked()
    apps = state["vision"]["apps"]
    candidates = [
        name for name, value in apps.items()
        if value.get("autostart") and value.get("desired_running")
    ]
    if not candidates:
        return None
    last_app = state["vision"].get("last_app")
    app_name = last_app if last_app in candidates else sorted(candidates)[0]
    value = apps[app_name]
    return {
        "app": app_name,
        "mode": value.get("mode", "deploy"),
        "config": value.get("config", "config.json"),
    }


def get_service_settings(key: str) -> Dict[str, bool]:
    if key not in _SERVICE_KEYS:
        raise KeyError(key)
    with _locked():
        state = _read_unlocked()
    return dict(state["services"][key])


def set_service_autostart(key: str, enabled: bool) -> Dict[str, bool]:
    if key not in _SERVICE_KEYS:
        raise KeyError(key)

    def change(state: Dict[str, Any]) -> None:
        state["services"][key]["autostart"] = bool(enabled)

    state = _update(change)
    return dict(state["services"][key])


def mark_service_started(key: str) -> None:
    if key not in _SERVICE_KEYS:
        raise KeyError(key)
    _update(lambda state: state["services"][key].update({"desired_running": True}))


def mark_service_stopped(key: str) -> None:
    if key not in _SERVICE_KEYS:
        raise KeyError(key)
    _update(lambda state: state["services"][key].update({"desired_running": False}))

