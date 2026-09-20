"""Usage:
  rkvision build [PROJECT] [--profile release|debug|relwithdebinfo] [-j N]
  rkvision build --all [--type app|example] [--profile release]

Build one or more visual programs while reusing the shared engine cache.
"""

from ..common import (
    PROJECT_WORKFLOW,
    add_build_arguments,
    prepare_build_args,
    run_for_projects,
    select_projects,
)


def register(subparsers) -> None:
    parser = subparsers.add_parser("build", help="编译视觉项目")
    parser.add_argument("target", nargs="?", help="项目名、应用 ID 或路径；项目目录内可省略")
    parser.add_argument("--all", action="store_true", help="编译全部项目")
    parser.add_argument("--type", choices=("app", "example"))
    add_build_arguments(parser)
    parser.set_defaults(command_handler=execute)


def execute(args) -> int:
    prepare_build_args(args)
    targets = select_projects(args.target, args.all, args.type)
    return run_for_projects(
        targets,
        lambda target: PROJECT_WORKFLOW.build(target.path, args),
        continue_on_error=args.all,
    )
