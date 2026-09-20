"""Usage:
  rkvision --help
  rkvision --version
  rkvision COMMAND [OPTIONS]

RKVision command registry and top-level error handling.
"""

from __future__ import annotations

import argparse
import subprocess
import sys

from . import __version__
from .argparse_zh import ChineseArgumentParser
from .common import CommandError, PROJECT_WORKFLOW
from .commands import (
    bench_cmd,
    build_cmd,
    check_cmd,
    clean_cmd,
    create_cmd,
    develop_cmd,
    doctor_cmd,
    hardware_cmd,
    install_cmd,
    list_cmd,
    logic_cmd,
    package_cmd,
    platform_cmd,
    network_cmd,
    run_cmd,
    runtime_cmd,
)


COMMAND_MODULES = (
    list_cmd,
    doctor_cmd,
    bench_cmd,
    create_cmd,
    check_cmd,
    build_cmd,
    run_cmd,
    runtime_cmd,
    package_cmd,
    clean_cmd,
    logic_cmd,
    install_cmd,
    platform_cmd,
    hardware_cmd,
    network_cmd,
    develop_cmd,
)


def build_parser() -> argparse.ArgumentParser:
    parser = ChineseArgumentParser(
        prog="rkvision",
        description="RKVision 统一项目、构建、运行与部署命令",
        epilog="项目参数可以是目录名、project.json 中的应用 ID 或完整路径。",
    )
    parser.add_argument("--version", action="version", version=f"RKVision {__version__()}")
    subparsers = parser.add_subparsers(dest="command", required=True)
    for module in COMMAND_MODULES:
        module.register(subparsers)
    return parser


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.command_handler(args) or 0)
    except KeyboardInterrupt:
        print("\n已取消", file=sys.stderr)
        return 130
    except subprocess.CalledProcessError as exc:
        command = " ".join(map(str, exc.cmd)) if isinstance(exc.cmd, (list, tuple)) else str(exc.cmd)
        print(f"错误：外部命令执行失败（退出码 {exc.returncode}）：{command}", file=sys.stderr)
        return exc.returncode or 1
    except FileNotFoundError as exc:
        target = exc.filename or str(exc)
        print(f"错误：找不到文件或命令：{target}", file=sys.stderr)
        return 1
    except PermissionError as exc:
        target = exc.filename or str(exc)
        print(f"错误：权限不足，无法访问：{target}", file=sys.stderr)
        return 1
    except (
        CommandError,
        RuntimeError,
        ValueError,
        PROJECT_WORKFLOW.ManifestError,
    ) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1
    except OSError as exc:
        detail = f"（系统错误码 {exc.errno}）" if exc.errno is not None else ""
        print(f"错误：系统操作失败{detail}", file=sys.stderr)
        return 1
