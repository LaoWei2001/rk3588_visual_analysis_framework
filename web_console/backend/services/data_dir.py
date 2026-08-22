"""Persistent runtime state for one installed application."""

import os
import shutil
from pathlib import Path


APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))


def data_dir(app_name: str) -> Path:
    return APPS_ROOT / ".data" / app_name


def ensure_data_dir(app_name: str) -> Path:
    directory = data_dir(app_name)
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def initialize_app_data(app_name: str, app_dir: Path) -> Path:
    """Create runtime-owned directories without copying package contract templates."""
    directory = ensure_data_dir(app_name)
    for child in ("event_store", "report_contracts", "contract_revisions"):
        (directory / child).mkdir(exist_ok=True)
    ota_source = app_dir / "services" / "model_update" / "ota_config.json"
    ota_destination = directory / "ota_config.json"
    if ota_source.is_file() and not ota_destination.exists():
        shutil.copy2(str(ota_source), str(ota_destination))
    return directory
