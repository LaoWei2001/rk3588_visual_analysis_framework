"""
ota_config.py — 网页管理「模型 OTA 升级服务」的配置。

用 JSON（而非 YAML）存, 这样 ota_agent.py 只用标准库 json 读取, 不给该服务新增依赖。

字段:
  platform_ws_host : 云平台 WebSocket 地址（不含协议/路径）

程序包中的 services/model_update/ota_config.json 是初始值；用户修改后保存在
APPS_ROOT/.data/{name}/ota_config.json。目标配置始终由当前运行实例决定。
"""

import json
import os
from pathlib import Path

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

from services.data_dir import data_dir, initialize_app_data

router = APIRouter()


class OtaConfigBody(BaseModel):
    platform_ws_host: str


def _config_path(name: str) -> Path:
    app_dir = APPS_ROOT / name
    if not app_dir.exists():
        raise HTTPException(status_code=404, detail=f"App '{name}' not found")
    initialize_app_data(name, app_dir)
    return data_dir(name) / "ota_config.json"


@router.get("/apps/{name}/ota-config")
async def get_ota_config(name: str):
    path = _config_path(name)
    if not path.exists():
        raise HTTPException(status_code=500, detail="当前应用包缺少模型 OTA 配置")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"解析 ota_config.json 失败: {e}")
    platform_ws_host = data.get("platform_ws_host") if isinstance(data, dict) else None
    if not isinstance(platform_ws_host, str) or not platform_ws_host.strip():
        raise HTTPException(status_code=500, detail="ota_config.json 缺少 platform_ws_host")
    return {"platform_ws_host": platform_ws_host.strip()}


@router.post("/apps/{name}/ota-config")
async def save_ota_config(name: str, body: OtaConfigBody):
    path = _config_path(name)
    platform_ws_host = body.platform_ws_host.strip()
    if not platform_ws_host:
        raise HTTPException(status_code=400, detail="平台 WebSocket 地址不能为空")
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    tmp.write_text(
        json.dumps({"platform_ws_host": platform_ws_host}, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    os.replace(tmp, path)
    return {"ok": True}
