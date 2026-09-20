"""Usage:
  rkvision doctor [--runtime-only] [--skip-app-check]

Inspect the RK3588 build/runtime environment without installing packages or
changing the system.
"""

import subprocess

from ..common import ENGINE_ROOT


def register(subparsers) -> None:
    parser = subparsers.add_parser("doctor", help="检查设备依赖与运行环境")
    parser.add_argument("--runtime-only", action="store_true")
    parser.add_argument("--skip-app-check", action="store_true")
    parser.set_defaults(command_handler=execute)


def execute(args) -> int:
    command = ["bash", str(ENGINE_ROOT / "setup/install_deps.sh"), "--check"]
    if args.runtime_only:
        command.append("--runtime-only")
    if args.skip_app_check:
        command.append("--skip-app-check")
    return subprocess.call(command, cwd=ENGINE_ROOT)
