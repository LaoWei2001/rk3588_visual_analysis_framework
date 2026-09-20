"""Usage:
  rkvision clean [PROJECT] [--profile release|debug|relwithdebinfo]
  rkvision clean --all [--type app|example]

Remove generated build output for the selected configuration only.
"""

import shutil

from ..common import (
    PROJECT_WORKFLOW,
    add_build_arguments,
    prepare_build_args,
    run_for_projects,
    select_projects,
)


def register(subparsers) -> None:
    parser = subparsers.add_parser("clean", help="清理项目构建产物")
    parser.add_argument("target", nargs="?", help="项目名、应用 ID 或路径；项目目录内可省略")
    parser.add_argument("--all", action="store_true", help="清理全部项目")
    parser.add_argument("--type", choices=("app", "example"))
    add_build_arguments(parser, clean=False)
    parser.set_defaults(command_handler=execute)


def execute(args) -> int:
    prepare_build_args(args)
    targets = select_projects(args.target, args.all, args.type)

    def clean_one(target) -> None:
        build_dir = PROJECT_WORKFLOW.build_path(target.path, args)
        if build_dir.exists():
            shutil.rmtree(build_dir)
            print(f"已清理 {build_dir}")
        else:
            print(f"无需清理 {build_dir}")

    return run_for_projects(targets, clean_one, continue_on_error=args.all)
