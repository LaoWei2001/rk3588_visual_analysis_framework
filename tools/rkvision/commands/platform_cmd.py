"""Usage:
  sudo rkvision platform install online|offline
  sudo rkvision platform upgrade [online|offline]
  rkvision platform status
  sudo rkvision platform uninstall [--yes]

Install, upgrade, inspect or uninstall the RKVision device platform.
"""

import subprocess

from ..common import ENGINE_ROOT


def register(subparsers) -> None:
    parser = subparsers.add_parser("platform", help="管理设备平台与 Web 服务")
    actions = parser.add_subparsers(dest="platform_command", required=True)

    install = actions.add_parser("install", help="安装平台")
    install.add_argument("mode", choices=("online", "offline"))
    install.set_defaults(command_handler=execute)

    upgrade = actions.add_parser("upgrade", help="升级平台")
    upgrade.add_argument("mode", choices=("online", "offline"), nargs="?", default="offline")
    upgrade.set_defaults(command_handler=execute)

    status = actions.add_parser("status", help="查看平台服务状态")
    status.set_defaults(command_handler=execute)

    uninstall = actions.add_parser("uninstall", help="卸载平台服务并保留应用数据")
    uninstall.add_argument("--yes", action="store_true")
    uninstall.set_defaults(command_handler=execute)


def execute(args) -> int:
    script = ENGINE_ROOT / "setup/install.sh"
    action = args.platform_command
    if action == "install":
        command = ["bash", str(script), args.mode]
    elif action == "upgrade":
        command = ["bash", str(script), "upgrade", args.mode]
    elif action == "uninstall":
        command = ["bash", str(script), "uninstall"]
        if args.yes:
            command.append("--yes")
    else:
        command = ["bash", str(script), "status"]
    return subprocess.call(command, cwd=ENGINE_ROOT)
