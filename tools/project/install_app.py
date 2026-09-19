#!/usr/bin/env python3
"""Install a package directory using the same lock and preservation policy as Web."""
import argparse
import os
from pathlib import Path
import re
import shutil
import sys
import tempfile

ENGINE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ENGINE / 'web_console/backend'))
from services import process_manager as pm
from services.package_install import replace_application


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('package', type=Path)
    parser.add_argument('--name')
    parser.add_argument('--replace-assets', action='store_true', help='explicitly replace on-device assets with package defaults')
    args = parser.parse_args()
    source = args.package.resolve()
    name = args.name or source.name
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]*', name):
        parser.error('invalid application name')
    if not (source / 'vision_analysis').is_file() or not (source / 'assets').is_dir():
        parser.error('package must contain vision_analysis and assets/')
    root = Path(os.environ.get('APPS_ROOT', '/opt/ai_apps')).resolve()
    root.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.app-install-', dir=root))
    try:
        shutil.copytree(source, staging, dirs_exist_ok=True)
        (staging / 'vision_analysis').chmod(0o755)
        with pm.runtime_lock():
            pm.stop_all_apps_for_install_unlocked()
            replace_application(staging, root / name, preserve_assets=not args.replace_assets)
    finally:
        if staging.exists(): shutil.rmtree(staging)
    print(f'Installed {root / name}; persistent data retained')


if __name__ == '__main__':
    main()
