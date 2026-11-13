"""Shared application and configuration path boundaries for HTTP routes."""
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
