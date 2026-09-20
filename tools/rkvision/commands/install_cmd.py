"""Usage:
  sudo rkvision install [PROJECT] [--name APP_ID] [--replace-assets]
                          [--profile release|debug|relwithdebinfo] [-j N]
  sudo rkvision install PACKAGE_DIR [--name APP_ID] [--replace-assets]

Resolve a project by directory name, application ID or path, build its package
and install it for the Web console. A complete package directory is installed
directly.
"""

from pathlib import Path

from ..common import (
    PROJECT_WORKFLOW,
    add_build_arguments,
    prepare_build_args,
    resolve_project,
)
from ..install_workflow import install


def register(subparsers) -> None:
    parser = subparsers.add_parser("install", help="自动构建并安装项目到 Web 控制台")
    parser.add_argument(
        "target",
        nargs="?",
        help="项目目录名、应用 ID、项目路径或完整发布包目录；项目目录内可省略",
    )
    parser.add_argument("--name")
    parser.add_argument("--replace-assets", action="store_true")
    add_build_arguments(parser)
    parser.set_defaults(command_handler=execute)


def _is_package(path: Path) -> bool:
    return (path / "vision_analysis").is_file() and (path / "assets").is_dir()


def _package_project(args) -> Path:
    target = resolve_project(args.target)
    print(f"已找到项目：{target.path}（应用 ID：{target.app_id}）")
    prepare_build_args(args)
    args.build_dir = None
    args.output = None
    args.no_strip = False
    args.no_bundle_libs = False
    return PROJECT_WORKFLOW.package(target.path, args)


def execute(args) -> int:
    requested = Path(args.target).expanduser() if args.target is not None else None
    if requested is not None and requested.is_dir() and _is_package(requested):
        package = requested.resolve()
        print(f"已找到发布包：{package}")
    else:
        package = _package_project(args)
    install(package, name=args.name, replace_assets=args.replace_assets)
    return 0
