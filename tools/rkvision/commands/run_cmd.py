"""Usage:
  rkvision run [PROJECT] [--config FILE] [--display|--no-display]
               [--profile release|debug|relwithdebinfo] [-j N]

Build and run a visual program in the foreground on a native RK3588 device.
The display switches are applied through a temporary runtime configuration;
the project's JSON file is never modified.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import platform
import tempfile

from ..common import (
    CommandError,
    ENGINE_ROOT,
    PROJECT_WORKFLOW,
    add_build_arguments,
    prepare_build_args,
    resolve_project,
)


def register(subparsers) -> None:
    parser = subparsers.add_parser("run", help="前台编译并运行视觉项目")
    parser.add_argument("target", nargs="?", help="项目名、应用 ID 或路径；项目目录内可省略")
    parser.add_argument("--config", type=Path, help="项目内配置文件，默认使用 project.json 声明")
    display = parser.add_mutually_exclusive_group()
    display.add_argument("--display", dest="display", action="store_true", help="本次运行开启 HDMI 显示")
    display.add_argument("--no-display", dest="display", action="store_false", help="本次运行关闭 HDMI 显示")
    parser.set_defaults(display=None)
    add_build_arguments(parser)
    parser.set_defaults(command_handler=execute)


def _config_path(project: Path, requested: Path | None) -> Path:
    if requested is None:
        requested = Path(PROJECT_WORKFLOW.project_info(project)["default_config"])
    path = requested if requested.is_absolute() else project / requested
    path = path.resolve()
    if project not in path.parents:
        raise CommandError("运行配置必须位于项目目录中")
    if not path.is_file():
        raise CommandError(f"找不到运行配置：{path}")
    return path


def _runtime_config(config: Path, display: bool | None):
    if display is None:
        return config, None
    try:
        value = json.loads(config.read_text(encoding="utf-8"))
        global_config = value.setdefault("global", {})
        if not isinstance(global_config, dict):
            raise ValueError("global 必须是对象")
        global_config["enable_display"] = 1 if display else 0
    except (OSError, ValueError, TypeError) as exc:
        raise CommandError(f"无法生成临时运行配置：{exc}") from exc
    handle = tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", prefix="rkvision-run-", suffix=".json", delete=False
    )
    with handle:
        json.dump(value, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    return Path(handle.name), Path(handle.name)


def execute(args) -> int:
    if platform.machine() not in ("aarch64", "armv7l") or args.toolchain or args.image:
        raise CommandError("run 只能在原生 RK3588 设备执行；其他主机请编译、打包后部署")
    prepare_build_args(args)
    target = resolve_project(args.target)
    output = PROJECT_WORKFLOW.build(target.path, args)
    config = _config_path(target.path, args.config)
    runtime_config, temporary = _runtime_config(config, args.display)
    environment = dict(os.environ)
    environment["ASSETS_DIR"] = str(target.path / "assets")
    runtime = ENGINE_ROOT / "vision_analysis/vendor/rknn/2.4.2a2/lib/aarch64"
    environment["LD_LIBRARY_PATH"] = str(runtime) + (
        ":" + environment["LD_LIBRARY_PATH"] if environment.get("LD_LIBRARY_PATH") else ""
    )
    try:
        PROJECT_WORKFLOW.run(
            [output / "vision_analysis", runtime_config], cwd=target.path, env=environment
        )
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return 0
