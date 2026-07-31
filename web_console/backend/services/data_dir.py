"""Persistent per-app data directory for files that must survive app updates.

config.yaml, contracts/, ota_config.json and event_store/ are user-editable
at runtime but previously lived inside the app directory.  Uploading a new
app package wipes the entire app directory, destroying those edits.

Now they live under  /opt/ai_apps/.data/{app_name}/  outside the app tree.
"""

import os
import shutil
from pathlib import Path

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))


def data_dir(app_name: str) -> Path:
    return APPS_ROOT / ".data" / app_name


def ensure_data_dir(app_name: str) -> Path:
    d = data_dir(app_name)
    d.mkdir(parents=True, exist_ok=True)
    return d


def _migrate_file(src: Path, dst: Path) -> None:
    """Copy *src* → *dst* once: only when *dst* does not already exist."""
    if not dst.exists() and src.is_file():
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(str(src), str(dst))


def _migrate_dir(src: Path, dst: Path) -> None:
    """Copy individual files from *src/* into *dst/* if *dst* is empty."""
    if dst.exists():
        return
    if not src.is_dir():
        return
    dst.mkdir(parents=True, exist_ok=True)
    any_copied = False
    for child in src.iterdir():
        if child.is_file():
            shutil.copy2(str(child), str(dst / child.name))
            any_copied = True
    if not any_copied:
        dst.rmdir()


def migrate_app_data(app_name: str, app_dir: Path) -> Path:
    """Ensure the persistent data directory exists and has initial files.

    On first call for a given *app_name* (data dir empty), the initial
    config / contracts are seeded from the current app directory.
    Subsequent calls are a no-op – user edits are never overwritten.
    """
    d = ensure_data_dir(app_name)

    _migrate_file(app_dir / "services" / "upload" / "config.yaml",
                  d / "upload_config.yaml")
    _migrate_dir(app_dir / "services" / "upload" / "contracts",
                 d / "contracts")
    _migrate_file(app_dir / "services" / "model_update" / "ota_config.json",
                  d / "ota_config.json")
    (d / "event_store").mkdir(exist_ok=True)

    return d
