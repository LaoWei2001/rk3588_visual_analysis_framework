"""Application package validation and installation shared by CLI and Web adapters.

Preparation completes before stopping apps. Directory replacement and rollback
run under the same device runtime lock used for starting applications.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import tempfile

from . import process_manager as pm


def validate_package(root: Path, *, binary_name: str = None) -> None:
    """Validate a directory package without executing its binary or stopping apps."""
    binary_name = pm.BINARY_NAME if binary_name is None else binary_name
    binary = root / binary_name
    if not binary.is_file() or binary.is_symlink():
        raise ValueError(f"程序包缺少主程序 {binary_name}")
    with binary.open('rb') as stream:
        header = stream.read(64)
    if (len(header) != 64 or header[:6] != b'\x7fELF\x02\x01'
            or struct.unpack_from('<HH', header, 16) not in ((2, 183), (3, 183))
            or struct.unpack_from('<H', header, 52)[0] != 64):
        raise ValueError("主程序必须是 RK3588 使用的 AArch64 ELF64 可执行文件")
    phoff = struct.unpack_from('<Q', header, 32)[0]
    phsize, phcount = struct.unpack_from('<HH', header, 54)
    if phoff < 64 or phsize != 56 or phcount == 0 or phoff + phsize * phcount > binary.stat().st_size:
        raise ValueError("主程序 ELF 加载头缺失或文件已截断")
    catalog = root / 'logics.json'
    if not catalog.is_file():
        raise ValueError("程序包缺少 logics.json 能力清单")
    document = json.loads(catalog.read_text(encoding='utf-8'))
    if (not isinstance(document, dict) or not isinstance(document.get('channel_logics'), list)
            or not isinstance(document.get('global_logics', []), list)):
        raise ValueError("logics.json 能力清单格式无效")
    configs = list((root / 'assets').glob('*.json'))
    valid = False
    for config in configs:
        try:
            data = json.loads(config.read_text(encoding='utf-8'))
        except (ValueError, OSError):
            if config.name == 'config.json':
                raise ValueError("默认配置 assets/config.json 无法解析")
            continue
        is_config = (isinstance(data, dict) and isinstance(data.get('channels'), list)
                     and isinstance(data.get('global', {}), dict))
        if config.name == 'config.json' and not is_config:
            raise ValueError("默认配置 assets/config.json 格式无效")
        valid = valid or is_config
    if not valid:
        raise ValueError("程序包 assets/ 中缺少有效的视觉程序配置")


def install(source: Path, name: str, *, apps_root: Path = None,
            binary_name: str = None, consume_source: bool = False) -> dict:
    """Install a directory package; consume_source is only for disposable staging.

    Explicit roots let adapters use their configured destination without mutating
    shared runtime globals. They must identify the same managed apps directory in
    production. Defaults are read at call time for source/deployed runtime parity.
    """
    checked_destination = pm._app_path(name)
    apps_root = pm.APPS_ROOT if apps_root is None else Path(apps_root)
    binary_name = pm.BINARY_NAME if binary_name is None else binary_name
    destination = apps_root / checked_destination.name
    validate_package(source, binary_name=binary_name)
    if destination.is_symlink():
        raise ValueError('目标应用目录不能是符号链接')
    apps_root.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='.app-install-', dir=apps_root))
    backup = staging / 'previous'
    prepared = staging / 'prepared'
    preserve_backup = False
    try:
        # A failed copy/validation must not interrupt a running application.
        # Web already extracts into private staging on this filesystem.
        if consume_source:
            source.rename(prepared)
        else:
            shutil.copytree(source, prepared)
        validate_package(prepared, binary_name=binary_name)
        (prepared / binary_name).chmod(0o755)
        with pm.runtime_lock():
            if destination.is_symlink():
                raise ValueError('目标应用目录不能是符号链接')
            stopped_apps = pm.stop_all_apps_for_install_unlocked()
            if destination.exists():
                destination.rename(backup)
                preserve_backup = True
            try:
                prepared.rename(destination)
            except OSError:
                if backup.exists():
                    try:
                        backup.rename(destination)
                    except OSError as rollback_error:
                        raise RuntimeError(f'安装与自动回滚均失败，旧程序已保留在 {backup}') from rollback_error
                preserve_backup = False
                raise
            preserve_backup = False
        return {
            'name': name,
            'has_binary': (destination / binary_name).is_file(),
            'has_config': (destination / 'assets/config.json').is_file(),
            'stopped_apps': stopped_apps,
        }
    finally:
        # A failed rollback keeps its backup even if a destination now exists.
        if not preserve_backup:
            shutil.rmtree(staging, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('name')
    args = parser.parse_args()
    install(args.source, args.name)


if __name__ == '__main__':
    main()
