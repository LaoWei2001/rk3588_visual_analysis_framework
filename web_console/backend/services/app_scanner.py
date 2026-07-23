import os
from pathlib import Path
from typing import List, Dict, Any

from services.process_manager import get_status
from services.runtime_state import get_vision_settings

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
BINARY_NAME = os.environ.get("BINARY_NAME", "vision_analysis")
CONSOLE_DIR = "_console"


def scan_apps() -> List[Dict[str, Any]]:
    apps = []
    if not APPS_ROOT.exists():
        return apps

    for entry in sorted(APPS_ROOT.iterdir()):
        if not entry.is_dir():
            continue
        if entry.name.startswith("_") or entry.name.startswith("."):
            continue

        binary = entry / BINARY_NAME
        assets_dir = entry / "assets"
        config = assets_dir / "config.json"   # config 统一放 assets/ 子目录

        models, labels, videos, config_files = [], [], [], []
        if assets_dir.exists():
            for f in sorted(assets_dir.iterdir()):
                rel = f"assets/{f.name}"
                if f.suffix == ".rknn":
                    models.append(rel)
                elif f.suffix == ".txt":
                    labels.append(rel)
                elif f.suffix in (".mp4", ".avi", ".mkv"):
                    videos.append(rel)
                elif f.suffix == ".json" and f.name != "roi_zones.json":
                    config_files.append(rel)   # 可选作启动配置的 .json（排除 ROI 数据）

        # 上次启动所用的配置文件名（供「指定配置启动」下拉默认选中），缺省 config.json
        cfg_file = entry / "run.config"
        active_config = "config.json"
        if cfg_file.exists():
            try:
                v = cfg_file.read_text().strip()
                if v:
                    active_config = v
            except OSError:
                pass

        status_info = get_status(entry.name)
        runtime_settings = get_vision_settings(entry.name)

        # 待上报记录数 = 统一事件目录数，每条事件必须包含 manifest.json。
        # 只检查目录结构、不解析内容；上报全部成功后事件目录会被删除。
        # ALARM_STORE_DIR 为全局覆盖(与 records.py 一致), 否则用 <app>/alarm_store。
        store = Path(os.environ["ALARM_STORE_DIR"]) if os.environ.get("ALARM_STORE_DIR") \
            else entry / "alarm_store"
        unreported = 0
        if store.is_dir():
            try:
                unreported = sum(
                    1 for event_dir in store.iterdir()
                    if event_dir.is_dir() and (event_dir / "manifest.json").is_file()
                )
            except OSError:
                unreported = 0

        apps.append({
            "name": entry.name,
            "path": str(entry),
            "has_binary": binary.exists(),
            "has_config": config.exists(),
            "models": models,
            "labels": labels,
            "videos": videos,
            "config_files": config_files,
            "active_config": active_config,
            "unreported": unreported,
            "autostart": runtime_settings["autostart"],
            "desired_running": runtime_settings["desired_running"],
            **status_info,
        })

    return apps
