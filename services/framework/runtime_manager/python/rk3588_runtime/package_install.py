"""Install a local application package under the device runtime lock."""
import argparse
from pathlib import Path
import shutil
import tempfile

from . import process_manager as pm


def install(source: Path, name: str):
    destination = pm._app_path(name)
    if not source.is_dir() or not (source / pm.BINARY_NAME).is_file():
        raise ValueError('程序包缺少视觉可执行文件')
    if destination.is_symlink():
        raise ValueError('目标应用目录不能是符号链接')
    pm.APPS_ROOT.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.app-install-', dir=pm.APPS_ROOT))
    backup = staging / 'previous'
    prepared = staging / 'prepared'
    try:
        # Copy first. An out-of-space error must not interrupt the running app.
        shutil.copytree(source, prepared)
        with pm.runtime_lock():
            pm.stop_all_apps_for_install_unlocked()
            if destination.exists():
                destination.rename(backup)
            try:
                prepared.rename(destination)
            except Exception:
                if backup.exists():
                    backup.rename(destination)
                raise
    finally:
        # Keep a failed rollback backup for manual recovery.
        if backup.exists() and not destination.exists():
            raise RuntimeError(f'安装失败，旧程序包保留在 {backup}')
        shutil.rmtree(staging, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('name')
    args = parser.parse_args()
    install(args.source, args.name)


if __name__ == '__main__':
    main()
