"""Usage:
  rkvision check [PROJECT]
  rkvision check --all [--type app|example]

Validate project metadata, Logic manifests and engine compatibility.
"""

from ..common import PROJECT_WORKFLOW, run_for_projects, select_projects


def register(subparsers) -> None:
    parser = subparsers.add_parser("check", help="校验项目及 Logic 清单")
    parser.add_argument("target", nargs="?", help="项目名、应用 ID 或路径；项目目录内可省略")
    parser.add_argument("--all", action="store_true", help="校验全部项目")
    parser.add_argument("--type", choices=("app", "example"))
    parser.set_defaults(command_handler=execute)


def execute(args) -> int:
    targets = select_projects(args.target, args.all, args.type)
    return run_for_projects(
        targets,
        lambda target: PROJECT_WORKFLOW.check(target.path),
        continue_on_error=args.all,
    )
