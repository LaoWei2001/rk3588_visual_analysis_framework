"""Usage:
  rkvision package [PROJECT] [--output DIR] [--profile release]
                   [--no-strip] [--no-bundle-libs]

Build and create an installable RKVision application directory and tarball.
"""

from pathlib import Path

from ..common import PROJECT_WORKFLOW, add_build_arguments, prepare_build_args, resolve_project


def register(subparsers) -> None:
    parser = subparsers.add_parser("package", help="构建并生成应用发布包")
    parser.add_argument("target", nargs="?", help="项目名、应用 ID 或路径；项目目录内可省略")
    parser.add_argument("--build-dir", type=Path, help="使用已完成且匹配的构建目录")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--no-strip", action="store_true")
    parser.add_argument("--no-bundle-libs", action="store_true")
    add_build_arguments(parser)
    parser.set_defaults(command_handler=execute)


def execute(args) -> int:
    prepare_build_args(args)
    target = resolve_project(args.target)
    PROJECT_WORKFLOW.package(target.path, args)
    return 0
