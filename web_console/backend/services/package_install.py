"""Filesystem part of application upgrades; caller holds the runtime lock and stops apps."""
from pathlib import Path
import shutil
import tempfile


def replace_application(staged: Path, destination: Path, preserve_assets: bool = True, binary_name: str = "vision_analysis") -> None:
    """Install a complete staged package; roll back the directory swap on failure.

    Existing assets win over bundled defaults, including OTA models and Web-created
    configurations. New asset names are added. Persistent .data is outside this
    operation. Explicit replacement is available to installers via preserve_assets.
    """
    if not (staged / binary_name).is_file():
        raise ValueError('Application package is missing vision_analysis')
    if not (staged / 'assets').is_dir():
        raise ValueError('Application package is missing assets/')
    if destination.exists() and preserve_assets:
        old_assets = destination / 'assets'
        if old_assets.is_dir():
            shutil.copytree(old_assets, staged / 'assets', dirs_exist_ok=True)
        old_selection = destination / 'run.config'
        if old_selection.is_file():
            shutil.copy2(old_selection, staged / 'run.config')
    # A unique backup avoids collisions between independent installation attempts.
    backup_parent = Path(tempfile.mkdtemp(prefix='.app-backup-', dir=destination.parent))
    backup = backup_parent / 'previous'
    try:
        if destination.exists():
            destination.rename(backup)
        try:
            staged.rename(destination)
        except BaseException:
            if backup.exists():
                backup.rename(destination)
            raise
    finally:
        # If rollback itself failed, retain the only old copy for recovery.
        if destination.exists() or not backup.exists():
            shutil.rmtree(backup_parent)
