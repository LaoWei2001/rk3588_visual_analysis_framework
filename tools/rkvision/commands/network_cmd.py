"""Usage:
  rkvision network build [--clean]
  sudo rkvision network configure [FIRST_NET_CONFIG_ARGS ...]

Build or launch the first-boot terminal network configuration tool.
"""

import argparse
import subprocess

from ..common import CommandError, ENGINE_ROOT


TOOL_DIR = ENGINE_ROOT / "tools/first_net_config"
BINARY = TOOL_DIR / "first_net_config"


def register(subparsers) -> None:
    parser = subparsers.add_parser("network", help="管理首次网络配置工具")
    actions = parser.add_subparsers(dest="network_command", required=True)

    build = actions.add_parser("build", help="构建网络配置工具")
    build.add_argument("--clean", action="store_true")
    build.set_defaults(command_handler=execute_build)

    configure = actions.add_parser("configure", help="启动网络配置界面")
    configure.add_argument("arguments", nargs=argparse.REMAINDER)
    configure.set_defaults(command_handler=execute_configure)


def execute_build(args) -> int:
    command = ["bash", str(TOOL_DIR / "build.sh")]
    if args.clean:
        command.append("clean")
    return subprocess.call(command, cwd=TOOL_DIR)


def execute_configure(args) -> int:
    if not BINARY.is_file():
        raise CommandError("网络配置工具尚未构建，请先执行：rkvision network build")
    return subprocess.call([str(BINARY), *args.arguments], cwd=TOOL_DIR)
