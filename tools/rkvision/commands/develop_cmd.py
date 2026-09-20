"""Usage:
  rkvision develop [--project PROJECT] [WIZARD OPTIONS] [DESCRIPTION ...]

Start the isolated AI-assisted Logic development workflow for a project.
The command keeps the development wizard's options under the unified entry.
"""

import subprocess
import sys

from ..common import CommandError, ENGINE_ROOT, resolve_project


def register(subparsers) -> None:
    parser = subparsers.add_parser("develop", help="启动隔离的 AI Logic 开发向导")
    parser.add_argument("--project", help="项目名、应用 ID 或路径；项目目录内可省略")
    parser.add_argument("--agent", choices=("auto", "codex", "claude"), default="auto")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--confirm-before-code", action="store_true")
    mode.add_argument("--plan-only", action="store_true")
    parser.add_argument("--model")
    parser.add_argument("--profile")
    parser.add_argument("--no-alt-screen", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("description", nargs="*")
    parser.set_defaults(command_handler=execute)


def execute(args) -> int:
    try:
        target = resolve_project(args.project)
    except CommandError:
        if args.project is not None:
            raise
        target = resolve_project("person_count")
    command = [
        sys.executable,
        str(ENGINE_ROOT / "docs/skills/rk3588-feature-wizard/scripts/start_wizard.py"),
        "--project",
        str(target.path),
    ]
    if args.agent != "auto":
        command.extend(("--agent", args.agent))
    if args.confirm_before_code:
        command.append("--confirm-before-code")
    if args.plan_only:
        command.append("--plan-only")
    if args.model:
        command.extend(("--model", args.model))
    if args.profile:
        command.extend(("--profile", args.profile))
    if args.no_alt_screen:
        command.append("--no-alt-screen")
    if args.check:
        command.append("--check")
    if args.dry_run:
        command.append("--dry-run")
    command.extend(args.description)
    return subprocess.call(command)
