"""
ota_config.py — 网页管理「模型 OTA 升级服务」的配置。

用 JSON（而非 YAML）存, 这样 ota_agent.py 只用标准库 json 读取, 不给该服务新增依赖。

字段:
  platform_ws_host : 云平台 WebSocket 地址（不含协议/路径）
  target_config    : OTA 要改写的配置文件名（相对 assets/），默认 config.json
                     —— 必须与控制台/ C++ 实际运行的那份一致, 否则换的模型热重载不进进程

文件位置: APPS_ROOT/{name}/services/model_update/ota_config.json
"""

import json
import os
from pathlib import Path
from typing import Any, Dict

from fastapi import APIRouter, HTTPException

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

router = APIRouter()

DEFAULT_OTA_CONFIG: Dict[str, Any] = {
    "platform_ws_host": "tunnel.memanager.cn",
    "target_config": "config.json",
}


def _config_path(name: str) -> Path:
    app_dir = APPS_ROOT / name
    if not app_dir.exists():
        raise HTTPException(status_code=404, detail=f"App '{name}' not found")
    return app_dir / "services" / "model_update" / "ota_config.json"


@router.get("/apps/{name}/ota-config")
async def get_ota_config(name: str):
    path = _config_path(name)
    if not path.exists():
        return DEFAULT_OTA_CONFIG
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"解析 ota_config.json 失败: {e}")
    return {**DEFAULT_OTA_CONFIG, **data}


@router.post("/apps/{name}/ota-config")
async def save_ota_config(name: str, body: Dict[str, Any]):
    path = _config_path(name)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(body, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp, path)
    return {"ok": True}
