import os
from pathlib import Path
from typing import List, Dict, Any

from services.process_manager import get_status

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
BINARY_NAME = os.environ.get("BINARY_NAME", "rk3588_yolo")
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

        # 未上报告警数 = 本地发件箱里 .json 元数据文件数(与 records.py 的 count 同口径)。
        # 只数文件、不解析内容 → 很轻; 上报微服务补传成功后会删除, 故这里是"还没传上去"的条数。
        # ALARM_STORE_DIR 为全局覆盖(与 records.py 一致), 否则用 <app>/alarm_store。
        store = Path(os.environ["ALARM_STORE_DIR"]) if os.environ.get("ALARM_STORE_DIR") \
            else entry / "alarm_store"
        unreported = 0
        if store.is_dir():
            try:
                unreported = sum(1 for f in store.iterdir()
                                 if f.suffix == ".json" and f.is_file())
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
            **status_info,
        })

    return apps
