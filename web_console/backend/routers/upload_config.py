"""
upload_config.py — 网页管理「上报服务」的 config.yaml（默认地址 / Redis / 超时）。

方案2 下, 每通道地址写在 config.json(编辑器管)；这里管的是 config.yaml 里的
「默认/兜底地址 + Redis 连接 + 超时」, 即通道留空时使用的值。

文件位置: APPS_ROOT/{name}/services/upload/config.yaml
"""

import os
from pathlib import Path
from typing import Any, Dict

import yaml
from fastapi import APIRouter, HTTPException

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

router = APIRouter()

# 文件不存在时返回的默认骨架（与 service/upload/config.yaml 结构一致）
DEFAULT_UPLOAD_CONFIG: Dict[str, Any] = {
    "dify":   {"api_url": "", "api_key": "", "timeout": 120},
    "server": {"url": "", "timeout": 15},
    "redis":  {"host": "localhost", "port": 6379, "db": 0,
               "dify_queue": "dify_queue", "server_queue": "server_queue"},
}


def _config_path(name: str) -> Path:
    app_dir = APPS_ROOT / name
    if not app_dir.exists():
        raise HTTPException(status_code=404, detail=f"App '{name}' not found")
    return app_dir / "services" / "upload" / "config.yaml"


@router.get("/apps/{name}/upload-config")
async def get_upload_config(name: str):
    path = _config_path(name)
    if not path.exists():
        return DEFAULT_UPLOAD_CONFIG
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"解析 config.yaml 失败: {e}")
    # 用默认值补齐缺失的小节，避免前端取不到键
    merged = {**DEFAULT_UPLOAD_CONFIG, **data}
    return merged


@router.post("/apps/{name}/upload-config")
async def save_upload_config(name: str, body: Dict[str, Any]):
    path = _config_path(name)
    path.parent.mkdir(parents=True, exist_ok=True)
    # 注意: PyYAML safe_dump 会丢掉原文件里的注释（如需保留改用 ruamel.yaml）
    text = yaml.safe_dump(body, allow_unicode=True, sort_keys=False)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, path)
    return {"ok": True}
