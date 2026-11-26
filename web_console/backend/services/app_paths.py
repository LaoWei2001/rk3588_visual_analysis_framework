"""Shared application and configuration path boundaries for HTTP routes."""
import json
from pathlib import Path

from fastapi import HTTPException


def resolve_application_directory(root: Path, name: str) -> Path:
    """Resolve an existing app without coupling service callers to HTTP errors."""
    if not name or Path(name).name != name or name in (".", ".."):
        raise ValueError("非法程序名称")
    app = (root / name).resolve()
    if app.parent != root.resolve():
        raise ValueError("程序路径越界")
    if not app.is_dir():
        raise FileNotFoundError(f"App '{name}' not found")
    return app


def application_directory(root: Path, name: str) -> Path:
    """HTTP adapter for the shared application directory boundary."""
    try:
        return resolve_application_directory(root, name)
    except ValueError as exc:
        raise HTTPException(400, str(exc)) from exc
    except FileNotFoundError as exc:
        raise HTTPException(404, str(exc)) from exc


def config_target(app: Path, path: str) -> Path:
    name = (path or "").strip().replace("\\", "/")
    if name.startswith("assets/"):
        name = name[len("assets/"):]
    assets = (app / "assets").resolve()
    target = (assets / name).resolve()
    if assets.parent != app.resolve() or target.parent != assets:
        raise HTTPException(400, "配置文件必须直接位于 assets/ 目录下")
    if target.suffix != ".json":
        raise HTTPException(400, "只允许访问 .json 配置文件")
    return target


def read_runtime_config(app: Path) -> dict:
    """Select the current instance's configuration, or the last selection while stopped."""
    from services.process_manager import get_status
    status = get_status(app.name)
    config_name = status.get("config")
    if not config_name:
        if status.get("launch_source") == "command_line":
            raise HTTPException(409, "命令行运行配置不在程序的 assets 配置列表中")
        try:
            config_name = (app / "run.config").read_text(encoding="utf-8").strip()
        except FileNotFoundError:
            pass
    target = config_target(app, config_name or "config.json")
    try:
        config = json.loads(target.read_text(encoding="utf-8"))
        if not isinstance(config, dict):
            raise ValueError("配置必须是 JSON 对象")
        return config
    except (ValueError, OSError) as exc:
        if status.get("launch_source") == "command_line":
            raise HTTPException(409, "无法读取命令行实例的运行配置") from exc
        return {}
