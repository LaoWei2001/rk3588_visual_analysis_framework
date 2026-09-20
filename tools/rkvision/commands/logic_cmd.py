"""Usage:
  rkvision logic add channel NAME [PROJECT] [--label LABEL]
  rkvision logic add global NAME [PROJECT] [--label LABEL]

Create and validate a channel or global Logic module in a visual project.
"""

from ..common import PROJECT_WORKFLOW, resolve_project
from ..logic_scaffold import main as scaffold_logic


def register(subparsers) -> None:
    parser = subparsers.add_parser("logic", help="管理项目 Logic 模块")
    actions = parser.add_subparsers(dest="logic_command", required=True)
    add = actions.add_parser("add", help="新建 Logic 模块")
    add.add_argument("kind", choices=("channel", "global"))
    add.add_argument("name")
    add.add_argument("target", nargs="?", help="项目名、应用 ID 或路径；项目目录内可省略")
    add.add_argument("--label")
    add.set_defaults(command_handler=execute_add)

def execute_add(args) -> int:
    target = resolve_project(args.target)
    PROJECT_WORKFLOW.project_info(target.path)
    command = [
        args.kind,
        args.name,
        "--project-root",
        target.path,
    ]
    if args.label:
        command.extend(("--label", args.label))
    result = scaffold_logic([str(item) for item in command])
    if result:
        return result
    PROJECT_WORKFLOW.check(target.path)
    return 0
