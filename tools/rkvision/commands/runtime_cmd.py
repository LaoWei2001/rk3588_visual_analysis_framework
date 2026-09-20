"""Usage:
  rkvision start APP [--mode deploy|debug] [--config FILE]
  rkvision stop APP
  rkvision restart APP [--mode deploy|debug] [--config FILE]
  rkvision status [APP] [--json]
  rkvision logs APP [--lines N] [--follow]
  rkvision autostart APP enable|disable

Control installed visual applications through the same process manager and
systemd units used by the Web console.
"""

from __future__ import annotations

import json
import re
import subprocess

from ..install_workflow import pm
from services import runtime_state


def _app_name(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]*", value):
        raise ValueError("应用名称只能包含字母、数字、下划线和连字符")
    return value


def _add_start_arguments(parser) -> None:
    parser.add_argument("app", type=_app_name)
    parser.add_argument("--mode", choices=("deploy", "debug"), default="deploy")
    parser.add_argument("--config", help="assets/ 下的配置文件名")


def register(subparsers) -> None:
    start = subparsers.add_parser("start", help="后台启动已安装视觉应用")
    _add_start_arguments(start)
    start.set_defaults(command_handler=execute_start)

    stop = subparsers.add_parser("stop", help="停止已安装视觉应用")
    stop.add_argument("app", type=_app_name)
    stop.set_defaults(command_handler=execute_stop)

    restart = subparsers.add_parser("restart", help="重启已安装视觉应用")
    _add_start_arguments(restart)
    restart.set_defaults(command_handler=execute_start)

    status = subparsers.add_parser("status", help="查看已安装视觉应用状态")
    status.add_argument("app", nargs="?", type=_app_name)
    status.add_argument("--json", action="store_true")
    status.set_defaults(command_handler=execute_status)

    logs = subparsers.add_parser("logs", help="读取或跟随视觉应用日志")
    logs.add_argument("app", type=_app_name)
    logs.add_argument("--lines", type=int, default=200)
    logs.add_argument("--follow", "-f", action="store_true")
    logs.set_defaults(command_handler=execute_logs)

    autostart = subparsers.add_parser("autostart", help="管理视觉应用开机自启")
    autostart.add_argument("app", type=_app_name)
    autostart.add_argument("state", choices=("enable", "disable"))
    autostart.set_defaults(command_handler=execute_autostart)


def _sync_services() -> None:
    from routers.services import sync_services_for_running_app

    result = sync_services_for_running_app()
    for message in result.get("errors", []):
        print(f"后台服务同步警告：{message}")


def execute_start(args) -> int:
    # The short-lived CLI must not orphan a journal follower; the Web process
    # attaches its own follower when it next discovers this shared unit.
    pid = pm.start_app(args.app, args.mode, args.config, follow_logs=False)
    _sync_services()
    print(f"已启动 {args.app}，PID={pid}，模式={args.mode}")
    return 0


def execute_stop(args) -> int:
    stopped = pm.stop_app(args.app)
    print(f"{'已停止' if stopped else '未在运行'} {args.app}")
    return 0


def execute_status(args) -> int:
    if args.app:
        values = {args.app: pm.get_status(args.app)}
    else:
        values = {}
        if pm.APPS_ROOT.is_dir():
            for path in sorted(pm.APPS_ROOT.iterdir()):
                if path.is_dir() and not path.name.startswith((".", "_")):
                    values[path.name] = pm.get_status(path.name)
    if args.json:
        print(json.dumps(values, ensure_ascii=False, indent=2))
        return 0
    if not values:
        print("没有已安装的视觉应用")
        return 0
    for name, value in values.items():
        detail = value.get("status", "unknown")
        if detail == "running":
            detail += f" pid={value.get('pid')} mode={value.get('mode')} config={value.get('config')}"
        print(f"{name}: {detail}")
    return 0


def execute_logs(args) -> int:
    if args.lines < 1:
        raise ValueError("--lines 必须为正数")
    for line in pm.get_logs(args.app, args.lines):
        print(line)
    if not args.follow:
        return 0
    return subprocess.call([
        "journalctl", "--quiet", "--follow", "--lines=0", "--output=cat",
        "--unit", pm.app_unit_name(args.app),
    ])


def execute_autostart(args) -> int:
    app_dir = pm.APPS_ROOT / args.app
    if not app_dir.is_dir():
        raise FileNotFoundError(f"应用不存在：{app_dir}")
    enabled = args.state == "enable"
    with pm.runtime_lock():
        value = runtime_state.set_vision_autostart(args.app, enabled)
    print(f"{args.app} 开机自启：{'已启用' if value['autostart'] else '已禁用'}")
    return 0
