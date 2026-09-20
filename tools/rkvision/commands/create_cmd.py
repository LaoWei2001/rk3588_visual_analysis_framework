"""Usage:
  rkvision create NAME
  rkvision create /absolute/or/relative/path

Create a self-contained RKVision application from the basic template.
"""

from ..common import PROJECT_WORKFLOW, create_destination


def register(subparsers) -> None:
    parser = subparsers.add_parser("create", help="创建视觉项目")
    parser.add_argument("destination", help="项目名或目标路径")
    parser.set_defaults(command_handler=execute)


def execute(args) -> int:
    PROJECT_WORKFLOW.create(create_destination(args.destination))
    return 0
