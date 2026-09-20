"""Install packages using the same lock and preservation policy as Web."""

import os
from pathlib import Path
import re
import shutil
import sys
import tempfile
from typing import Optional

ENGINE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ENGINE / 'web_console/backend'))
from services import process_manager as pm
from services.package_install import replace_application


def install(package: Path, name: Optional[str] = None, replace_assets: bool = False) -> Path:
    source = package.resolve()
    name = name or source.name
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_-]*', name):
        raise ValueError('应用名称无效，只能包含字母、数字、下划线和连字符')
    if not (source / 'vision_analysis').is_file() or not (source / 'assets').is_dir():
        raise ValueError('发布包必须包含 vision_analysis 和 assets/ 目录')
    root = Path(os.environ.get('APPS_ROOT', '/opt/ai_apps')).resolve()
    root.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.app-install-', dir=root))
    try:
        shutil.copytree(source, staging, dirs_exist_ok=True)
        (staging / 'vision_analysis').chmod(0o755)
        with pm.runtime_lock():
            pm.stop_all_apps_for_install_unlocked()
            replace_application(staging, root / name, preserve_assets=not replace_assets)
    finally:
        if staging.exists(): shutil.rmtree(staging)
    destination = root / name
    print(f'已安装到 {destination}；已保留持久化数据')
    return destination
