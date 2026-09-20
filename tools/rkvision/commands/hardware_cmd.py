"""Usage:
  rkvision hardware build gpio|relay
  sudo rkvision hardware gpio [GPIOCTL_ARGS ...]
  rkvision hardware relay [RELAY_TEST_ARGS ...]

Build or run the board GPIO and relay diagnostic tools.
"""

import argparse
import subprocess

from ..common import CommandError, ENGINE_ROOT


TOOLS = {
    "gpio": (
        ENGINE_ROOT / "tools/hardware/gpio_test/build.sh",
        ENGINE_ROOT / "tools/hardware/gpio_test/build/rk3588-gpioctl",
    ),
    "relay": (
        ENGINE_ROOT / "tools/hardware/relay_test/build.sh",
        ENGINE_ROOT / "tools/hardware/relay_test/build/relay_test",
    ),
}


def register(subparsers) -> None:
    parser = subparsers.add_parser("hardware", help="构建或运行硬件诊断工具")
    actions = parser.add_subparsers(dest="hardware_command", required=True)

    build = actions.add_parser("build", help="构建硬件工具")
    build.add_argument("tool", choices=tuple(TOOLS))
    build.set_defaults(command_handler=execute_build)

    for name in TOOLS:
        run = actions.add_parser(name, help=f"运行 {name} 工具")
        run.add_argument("arguments", nargs=argparse.REMAINDER)
        run.set_defaults(command_handler=execute_run, tool=name)


def execute_build(args) -> int:
    script, _ = TOOLS[args.tool]
    return subprocess.call(["bash", str(script)], cwd=script.parent)


def execute_run(args) -> int:
    _, binary = TOOLS[args.tool]
    if not binary.is_file():
        raise CommandError(f"工具尚未构建，请先执行：rkvision hardware build {args.tool}")
    return subprocess.call([str(binary), *args.arguments], cwd=binary.parent.parent)
