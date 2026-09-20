"""Usage:
  rkvision list [--type app|example|external] [--json]

List visual applications and examples known to the current workspace.
"""

from __future__ import annotations

import json

from ..common import discover_projects


def register(subparsers) -> None:
    parser = subparsers.add_parser("list", help="列出视觉项目和示例")
    parser.add_argument("--type", choices=("app", "example", "external"))
    parser.add_argument("--json", action="store_true", help="输出机器可读 JSON")
    parser.set_defaults(command_handler=execute)


def execute(args) -> int:
    targets = discover_projects()
    if args.type:
        targets = [target for target in targets if target.kind == args.type]
    if args.json:
        print(json.dumps([
            {
                "type": item.kind,
                "name": item.name,
                "id": item.app_id,
                "version": item.version,
                "path": str(item.path),
            }
            for item in targets
        ], ensure_ascii=False, indent=2))
        return 0
    if not targets:
        print("没有发现项目")
        return 0
    widths = {
        "kind": max(4, *(len(item.kind) for item in targets)),
        "name": max(4, *(len(item.name) for item in targets)),
        "id": max(2, *(len(item.app_id) for item in targets)),
    }
    print(f"{'TYPE':<{widths['kind']}}  {'NAME':<{widths['name']}}  {'ID':<{widths['id']}}  PATH")
    for item in targets:
        relative = item.path.relative_to(item.path.parents[1]) if len(item.path.parents) > 1 else item.path
        print(f"{item.kind:<{widths['kind']}}  {item.name:<{widths['name']}}  {item.app_id:<{widths['id']}}  {relative}")
    return 0
