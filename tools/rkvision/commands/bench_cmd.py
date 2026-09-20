"""Usage:
  rkvision bench compare BEFORE_BUILD AFTER_BUILD [AFTER_BUILD ...]
  rkvision bench run --before BIN --after BIN --config FILE --output DIR [OPTIONS]
  rkvision bench sweep MODEL [SECONDS] [CORE_MODE] [READBACK]
  sudo rkvision bench frequency status|lock|restore

Compare code output, measure application performance, sweep NPU contexts or
inspect and control benchmark-only device frequencies.
"""

import subprocess
from pathlib import Path

from .. import benchmark_workflow, code_compare
from ..common import ENGINE_ROOT


def register(subparsers) -> None:
    parser = subparsers.add_parser("bench", help="运行性能和构建一致性工具")
    actions = parser.add_subparsers(dest="bench_command", required=True)

    compare = actions.add_parser("compare", help="比较构建产物的 ELF 可执行段")
    compare.add_argument("before", type=Path)
    compare.add_argument("after", type=Path, nargs="+")
    compare.set_defaults(command_handler=execute_compare)

    run = actions.add_parser("run", help="交替测量两个视觉程序")
    run.add_argument("--before", type=Path, required=True)
    run.add_argument("--after", type=Path, required=True)
    run.add_argument("--config", type=Path, required=True)
    run.add_argument("--output", type=Path, required=True)
    run.add_argument("--cwd", type=Path, default=Path.cwd())
    run.add_argument("--seconds", type=int, default=32)
    run.add_argument("--warmup-seconds", type=int, default=5)
    run.set_defaults(command_handler=execute_run)

    sweep = actions.add_parser("sweep", help="扫描 NPU context 吞吐拐点")
    sweep.add_argument("model", type=Path)
    sweep.add_argument("seconds", nargs="?", type=int, default=10)
    sweep.add_argument("core_mode", nargs="?", type=int, default=0)
    sweep.add_argument("readback", nargs="?", type=int, default=0)
    sweep.set_defaults(command_handler=execute_sweep)

    frequency = actions.add_parser("frequency", help="查看、锁定或恢复实验频率")
    frequency.add_argument("action", choices=("status", "lock", "restore"))
    frequency.set_defaults(command_handler=execute_frequency)


def execute_compare(args) -> int:
    return code_compare.main([str(args.before), *(str(path) for path in args.after)])


def execute_run(args) -> int:
    return benchmark_workflow.main([
        "--before", str(args.before),
        "--after", str(args.after),
        "--config", str(args.config),
        "--output", str(args.output),
        "--cwd", str(args.cwd),
        "--seconds", str(args.seconds),
        "--warmup-seconds", str(args.warmup_seconds),
    ])


def execute_sweep(args) -> int:
    script = ENGINE_ROOT / "vision_analysis/bench/bench_sweep.sh"
    return subprocess.call([
        "bash", str(script), str(args.model), str(args.seconds),
        str(args.core_mode), str(args.readback),
    ], cwd=script.parent)


def execute_frequency(args) -> int:
    script = ENGINE_ROOT / "vision_analysis/bench/npu_freq.sh"
    return subprocess.call(["bash", str(script), args.action], cwd=script.parent)
