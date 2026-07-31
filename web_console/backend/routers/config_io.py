import json
import os
import subprocess
from pathlib import Path
from typing import Any, Dict

from fastapi import APIRouter, HTTPException
from fastapi.responses import JSONResponse

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

router = APIRouter()

KNOWN_MODEL_TYPES = ["yolov5", "yolov8_det", "yolov8_pose", "yolov5_seg"]


def _app_dir(name: str) -> Path:
    p = APPS_ROOT / name
    if not p.exists():
        raise HTTPException(status_code=404, detail=f"App '{name}' not found")
    return p


def _atomic_write(path: Path, data: Any) -> None:
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=4), encoding="utf-8")
    os.replace(tmp, path)


def _safe_config_target(app_dir: Path, path: str) -> Path:
    """把前端传入的相对路径解析为 assets/ 下的一个合法配置文件路径。

    约束（防路径穿越/误写）：必须落在 assets/ 内且后缀为 .json。
    接受 'config.json' / 'assets/config.json' 两种写法。
    """
    name = (path or "").strip().replace("\\", "/")
    if name.startswith("assets/"):
        name = name[len("assets/"):]
    assets = (app_dir / "assets").resolve()
    target = (assets / name).resolve()
    if assets != target.parent:
        raise HTTPException(status_code=400, detail="配置文件必须直接位于 assets/ 目录下")
    if target.suffix != ".json":
        raise HTTPException(status_code=400, detail="只允许保存 .json 配置文件")
    return target


@router.get("/console/info")
async def console_info():
    return {
        "version": "1.0.0",
        "apps_root": str(APPS_ROOT),
        "binary_name": os.environ.get("BINARY_NAME", "vision_analysis"),
        "known_model_types": KNOWN_MODEL_TYPES,
    }


@router.get("/apps/{name}/logics")
async def get_app_logics(name: str):
    """动态获取该 App 可用的 logic 清单。

    优先级: logics.json > 二进制 --list-logics。两者都不可用时返回空清单和
    明确错误，不回退到可能已经过期的硬编码通道逻辑名。
    """
    app_dir = _app_dir(name)
    errors = []

    # 1. logics.json（通道/全局 ID 来自各自注册宏，其余元数据由模块 logic.json 聚合）
    logics_file = app_dir / "logics.json"
    if logics_file.exists():
        try:
            data = json.loads(logics_file.read_text(encoding="utf-8"))
            channel_logics = data.get("channel_logics") if isinstance(data, dict) else None
            global_logics = data.get("global_logics") if isinstance(data, dict) else None
            if not isinstance(channel_logics, list):
                errors.append("logics.json 缺少有效的 channel_logics 数组")
            elif not isinstance(global_logics, list):
                errors.append("logics.json 缺少有效的 global_logics 数组")
            else:
                model_types = data.get("model_types", KNOWN_MODEL_TYPES)
                return {
                    "channel_logics": channel_logics,
                    "global_logics": global_logics,
                    "model_types": (
                        model_types
                        if isinstance(model_types, list)
                        else KNOWN_MODEL_TYPES
                    ),
                    "source": "catalog",
                }
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            errors.append(f"logics.json 读取失败: {exc}")
    else:
        errors.append("应用包中没有 logics.json")

    # 2. 尝试运行二进制 --list-logics
    binary_name = os.environ.get("BINARY_NAME", "vision_analysis")
    binary_path = app_dir / binary_name
    if binary_path.exists():
        try:
            channel_result = subprocess.run(
                [str(binary_path), "--list-logics"],
                capture_output=True, text=True, timeout=3, cwd=str(app_dir),
            )
            global_result = subprocess.run(
                [str(binary_path), "--list-global-logics"],
                capture_output=True, text=True, timeout=3, cwd=str(app_dir),
            )
            if channel_result.returncode == 0 and global_result.returncode == 0:
                channel_lines = [
                    ln.strip() for ln in channel_result.stdout.splitlines() if ln.strip()
                ]
                global_lines = [
                    ln.strip() for ln in global_result.stdout.splitlines() if ln.strip()
                ]
                return {
                    "channel_logics": channel_lines,
                    "global_logics": global_lines,
                    "model_types":    KNOWN_MODEL_TYPES,
                    "source": "binary",
                }
            errors.append(
                "二进制 logic 清单读取失败"
                f"（channel={channel_result.returncode}, global={global_result.returncode}）"
            )
        except subprocess.TimeoutExpired:
            errors.append("二进制 --list-logics 执行超时")
        except (OSError, UnicodeError) as exc:
            errors.append(f"二进制 --list-logics 无法执行: {exc}")
    else:
        errors.append(f"应用包中没有可执行文件 {binary_name}")

    # 不返回静态逻辑名，避免已删除模块重新出现在 Web 下拉框中。
    return {
        "channel_logics": [],
        "global_logics":  [],
        "model_types":    KNOWN_MODEL_TYPES,
        "source": "unavailable",
        "error": "；".join(errors),
    }


@router.get("/apps/{name}/config-files")
async def list_config_files(name: str):
    """List importable JSON config files from assets/."""
    app_dir    = _app_dir(name)
    assets_dir = app_dir / "assets"
    if not assets_dir.exists():
        return []
    files = sorted(
        str(f.relative_to(app_dir))
        for f in assets_dir.glob("*.json")
    )
    return files


@router.get("/apps/{name}/config-file")
async def load_config_file(name: str, path: str):
    """Load a specific JSON file from within the app directory."""
    app_dir = _app_dir(name)
    target = (app_dir / path).resolve()
    if not str(target).startswith(str(app_dir.resolve())):
        raise HTTPException(status_code=400, detail="Path traversal not allowed")
    if not target.exists():
        raise HTTPException(status_code=404, detail="File not found")
    return json.loads(target.read_text(encoding="utf-8"))


@router.get("/apps/{name}/config")
async def get_config(name: str):
    app_dir     = _app_dir(name)
    config_file = app_dir / "assets" / "config.json"
    if not config_file.exists():
        return JSONResponse(content=None)
    return json.loads(config_file.read_text(encoding="utf-8"))


@router.post("/apps/{name}/config")
async def save_config(name: str, body: Dict[str, Any]):
    app_dir    = _app_dir(name)
    assets_dir = app_dir / "assets"
    assets_dir.mkdir(exist_ok=True)
    _atomic_write(assets_dir / "config.json", body)
    return {"ok": True}


@router.post("/apps/{name}/config-file")
async def save_config_file(name: str, path: str, body: Dict[str, Any]):
    """保存配置到 assets/ 下指定文件（用于「另存为」/编辑非默认配置）。

    path 可为 'config_test.json' 或 'assets/config_test.json'；目标不存在则新建。
    与 config.json 互不影响——这样可以在副本上改而不破坏原配置。
    """
    app_dir = _app_dir(name)
    target  = _safe_config_target(app_dir, path)
    target.parent.mkdir(exist_ok=True)
    _atomic_write(target, body)
    return {"ok": True, "path": str(target.relative_to(app_dir)).replace("\\", "/")}


@router.delete("/apps/{name}/config-file")
async def delete_config_file(name: str, path: str):
    """删除 assets/ 下指定配置文件。"""
    app_dir = _app_dir(name)
    target  = _safe_config_target(app_dir, path)
    if not target.exists():
        raise HTTPException(status_code=404, detail="文件不存在")
    target.unlink()
    # 若删除的恰是「上次启动配置」记录指向的文件，清掉记录，避免下次按不存在的配置启动
    run_cfg = app_dir / "run.config"
    try:
        if run_cfg.exists() and run_cfg.read_text().strip() == target.name:
            run_cfg.unlink()
    except OSError:
        pass
    return {"ok": True}
