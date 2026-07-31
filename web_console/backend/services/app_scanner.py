import os
from pathlib import Path
from typing import List, Dict, Any

from services.process_manager import get_status
from services.runtime_state import get_vision_settings
from services.data_dir import data_dir

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
                elif f.suffix == ".json":
                    config_files.append(rel)

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

        # 待上报记录数 = 统一事件目录数，以 event.json 为完成标记。
        # 只检查目录结构、不解析内容；上报全部成功后事件目录会被删除。
        # 与 records.py / process_manager.py 保持一致，使用 data_dir 下的 event_store。
        store = data_dir(entry.name) / "event_store"
        unreported = 0
        if store.is_dir():
            try:
                unreported = sum(
                    1 for event_dir in store.iterdir()
                    if event_dir.is_dir() and (event_dir / "event.json").is_file()
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
