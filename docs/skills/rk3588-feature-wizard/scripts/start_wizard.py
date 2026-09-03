#!/usr/bin/env python3
"""Start a platform-aware Codex or Claude Code feature-development session."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
from typing import Callable, Sequence
import uuid


sys.dont_write_bytecode = True
SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from agent_adapters import (  # noqa: E402
    AGENT_CHOICES,
    AGENT_DEFINITIONS,
    AdapterError,
    AgentRuntime,
    build_agent_command,
    claude_bash_sandbox_ready,
    decode_process_output,
    discover_builtin_agents,
    inspect_builtin_agent,
    permission_summary,
)
from write_guard import (  # noqa: E402
    IsolatedLogicWorkspace,
    WriteBoundaryError,
    allowed_roots_text,
)


SKILL_RELATIVE = Path("docs/skills/rk3588-feature-wizard/SKILL.md")
REQUIRED_SKILLS = (
    "rk3588-feature-wizard",
    "build-rk3588-vision-app",
    "rk3588-channel-logic",
    "rk3588-global-logic",
    "rk3588-console-ops",
    "rk3588-src-modules",
)
REQUIRED_SUPPORT_FILES = (
    Path("docs/skills/rk3588-feature-wizard/scripts/agent_adapters.py"),
    Path("docs/skills/rk3588-feature-wizard/scripts/write_guard.py"),
)
TOOL_COMMANDS = {
    "rg": ("rg",),
    "bash": ("bash",),
    "cmake": ("cmake",),
    "docker": ("docker",),
    "node": ("node",),
    "npm": ("npm",),
    "systemctl": ("systemctl",),
    "gstreamer": ("gst-launch-1.0",),
}
CLAUDE_TURN_SCHEMA = json.dumps(
    {
        "type": "object",
        "properties": {
            "state": {"type": "string", "enum": ["wait", "done"]},
            "message": {"type": "string"},
        },
        "required": ["state", "message"],
        "additionalProperties": False,
    },
    ensure_ascii=False,
    separators=(",", ":"),
)
CLAUDE_TURN_PROTOCOL = """

本会话由启动器通过 Claude Code 非交互多轮模式转发，以跳过一次性隔离目录的信任弹窗。不要调用
AskUserQuestion；需要用户回答时直接在本轮最终结果中提问。每轮必须返回结构化结果：
- `state=wait`：仍需用户回答；`message` 只放本轮说明和一个清晰问题；
- `state=done`：实现/计划已经完成，或因 Logic-only 边界停止；`message` 放完整交付或阻塞结论。
不能为了结束会话而提前使用 `done`，也不能在 `done` 后继续索取用户输入。
""".strip()


@dataclass(frozen=True)
class HostEnvironment:
    kind: str
    label: str
    system: str
    release: str
    machine: str
    python_version: str
    present_tools: tuple[str, ...]
    missing_tools: tuple[str, ...]
    recommendation: str

    def report_block(self) -> str:
        present = ", ".join(self.present_tools) or "无"
        missing = ", ".join(self.missing_tools) or "无"
        return "\n".join(
            (
                f"- 自动判定类型：{self.label}（{self.kind}）",
                f"- 系统原始信息：{self.system} {self.release} / {self.machine}",
                f"- Python：{self.python_version}",
                f"- 已发现工具：{present}",
                f"- 未发现的可选工具：{missing}",
                f"- 当前主机建议：{self.recommendation}",
            )
        )


@dataclass(frozen=True)
class AgentProbe:
    key: str
    executable: str | None
    runtime: AgentRuntime | None
    error: str | None

    @property
    def usable(self) -> bool:
        return self.runtime is not None


def classify_host(
    system: str,
    machine: str,
    release: str,
    kernel_release: str,
    device_compatible: str,
    environ: dict[str, str],
) -> tuple[str, str, str]:
    """Return a stable host kind, label, and recommendation from observed facts."""
    system_lower = system.lower()
    machine_lower = machine.lower()
    linux_release = f"{release} {kernel_release}".lower()
    compatible_lower = device_compatible.lower()
    wsl_hint = "microsoft" in linux_release or bool(environ.get("WSL_DISTRO_NAME"))

    if system_lower == "windows":
        return (
            "windows-native",
            "原生 Windows",
            "使用 PowerShell/CMD 原生命令；Linux build.sh、systemd、RKNN/RGA 和板端硬件验证转到 WSL2、交叉编译环境或 RK3588 板端。",
        )
    if system_lower == "darwin":
        return (
            "macos",
            "macOS",
            "可进行源码、文档及具备依赖的 Python/Web 验证；Linux 服务、RKNN/RGA 和硬件验证转到 Linux 或 RK3588 板端。",
        )
    if system_lower == "linux" and wsl_hint:
        if "wsl2" in linux_release:
            return (
                "windows-wsl2",
                "Windows WSL2",
                "按 Linux 命令工作；适合源码和多数非硬件验证，RKNN/RGA/GPIO/真实视频设备仍需按实际透传能力或 RK3588 板端验收。",
            )
        return (
            "windows-wsl-unknown",
            "Windows WSL（版本未确认）",
            "先确认 WSL 版本和所选编程代理的支持边界；优先改用 WSL2 后再按 Linux 工具链规划验证。",
        )
    if system_lower == "linux" and "rk3588" in compatible_lower:
        return (
            "linux-rk3588",
            "Linux RK3588 板端",
            "可在依赖、模型、视频源和设备节点齐备时执行板端构建与硬件验证；不得仅凭架构或设备树宣称具体外设可用。",
        )
    if system_lower == "linux" and machine_lower in {"x86_64", "amd64"}:
        return (
            "linux-x86-64",
            "Linux x86_64",
            "可完成源码、文档、Python/Web 和清单验证；引擎使用已配置的交叉编译环境，RKNN/RGA/GPIO 留到 RK3588 板端。",
        )
    if system_lower == "linux" and machine_lower in {"aarch64", "arm64"}:
        return (
            "linux-arm64",
            "Linux ARM64（未确认 RK3588）",
            "先确认目标芯片和 RKNN/RGA 依赖；可运行通用检查，但不得把 ARM64 等同于 RK3588 板端。",
        )
    if system_lower == "linux":
        return (
            "linux-other",
            "Linux（其他架构）",
            "可运行当前主机具备依赖的通用检查；RK3588 构建和硬件验证需另行确认。",
        )
    return (
        "other",
        f"未专门适配的系统：{system or 'unknown'}",
        "先使用 plan-only；确认可用 shell、Python、编程代理和目标构建环境后再允许自动实现。",
    )


def read_optional_text(paths: Sequence[Path], binary: bool = False) -> str:
    for path in paths:
        try:
            if binary:
                return path.read_bytes().replace(b"\x00", b",").decode(
                    "utf-8", errors="replace"
                )
            return path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
    return ""


def detected_tools() -> tuple[tuple[str, ...], tuple[str, ...]]:
    present = []
    missing = []
    for label, commands in TOOL_COMMANDS.items():
        target = present if any(shutil.which(command) for command in commands) else missing
        target.append(label)
    return tuple(present), tuple(missing)


def detect_host_environment() -> HostEnvironment:
    system = platform.system() or "unknown"
    machine = platform.machine() or "unknown"
    release = platform.release() or "unknown"
    kernel_release = read_optional_text((Path("/proc/sys/kernel/osrelease"),)).strip()
    device_compatible = read_optional_text(
        (
            Path("/proc/device-tree/compatible"),
            Path("/sys/firmware/devicetree/base/compatible"),
        ),
        binary=True,
    )
    kind, label, recommendation = classify_host(
        system,
        machine,
        release,
        kernel_release,
        device_compatible,
        dict(os.environ),
    )
    present, missing = detected_tools()
    return HostEnvironment(
        kind=kind,
        label=label,
        system=system,
        release=release,
        machine=machine,
        python_version=platform.python_version(),
        present_tools=present,
        missing_tools=missing,
        recommendation=recommendation,
    )


def repository_root(start: Path) -> Path:
    """Locate this project from its own files without requiring Git metadata."""
    resolved = start.resolve()
    for candidate in (resolved, *resolved.parents):
        if (
            (candidate / "develop_feature").is_file()
            and (candidate / "vision_analysis" / "src").is_dir()
            and (candidate / "docs" / "skills").is_dir()
        ):
            return candidate
    raise RuntimeError("无法从开发向导位置找到项目根目录")


def missing_skill_files(repo: Path) -> list[Path]:
    missing = [
        repo / "docs" / "skills" / name / "SKILL.md"
        for name in REQUIRED_SKILLS
        if not (repo / "docs" / "skills" / name / "SKILL.md").is_file()
    ]
    missing.extend(
        repo / relative
        for relative in REQUIRED_SUPPORT_FILES
        if not (repo / relative).is_file()
    )
    return missing


def build_prompt(
    description: str,
    mode: str,
    host: HostEnvironment,
    agent_label: str,
    agent_permission: str,
    agent_key: str | None = None,
) -> str:
    initial = description or "用户尚未提供初始功能描述；确认环境后，再询问希望实现什么业务结果。"
    mode_rules = {
        "auto": (
            "运行模式为 auto。满足完整性门后，展示需求合同并在同一会话立即开始实现，"
            "不要再索要笼统确认。"
        ),
        "confirm": (
            "运行模式为 confirm。满足完整性门后，展示需求合同和预计文件，等待用户明确确认后再实现。"
        ),
        "plan-only": (
            "运行模式为 plan-only。只完成需求澄清、源码核对、需求合同和实施计划，禁止修改文件。"
        ),
    }
    prompt = f"""你正在为 rk3588_visual_analysis_framework 启动交互式功能开发向导。

首先完整读取 `{SKILL_RELATIVE.as_posix()}` 并严格执行。它是本仓库的总控工作流；即使当前代理不会
自动发现或调用名为 `rk3588-feature-wizard` 的 Skill，也必须按文件内容执行，不得简化流程。
{mode_rules[mode]}

当前编程代理：{agent_label}
启动权限边界：{agent_permission}

初始需求：
{initial}

启动器自动检测到以下开发宿主信息；这是观测结果，不等同于最终部署/验收环境：
{host.report_block()}

第一轮必须先展示上述检测结果，并把 RK3588 Linux 写成默认部署建议，然后只问：“检测结果和默认部署
目标是否正确？”提示用户回复“是”即可；不正确可回复“否 + 简短纠正”。不要要求用户填写工具、模型、
摄像头、NPU、GPIO 或远端服务清单，也不要给长篇回答模板。用户只答“否”时，最多再给一个短选项题。
在环境确认前不要询问业务功能或修改文件。

这是一个 Logic-only 隔离开发会话。原仓库唯一允许写回的路径是：{allowed_roots_text()}。可以只读检查
仓库其他位置，但禁止修改 `vision_analysis/src/logic/core/**`、配置、测试、文档、Web、服务、脚本、
生成物或任何其他路径；也禁止创建符号链接。这个限制高于其他 Skill 中关于同步 Web、框架或文档的建议。
如果需求无法完全通过现有公共 API、模块 `logic.json` 和模块内 `report_templates/` 实现，明确说明受限原因
并停止，不得请求扩大权限或尝试越界。代理运行在一次性隔离副本中；不要查找、访问或修改其他仓库副本。
当前副本是普通项目目录，不要求 Git；不要初始化 `.git`，也不要把 Git 状态作为实施前提。

权限已经由启动器配置为白名单内自动执行。不要要求用户运行 `/permissions`，不要请求 Full Access、
danger-full-access、bypassPermissions 或任何权限升级；命令被边界拒绝时，缩小验证范围并如实报告。

此后按总控文件进入精简需求发现：通常只问 2–3 个业务主问题，硬上限 4 个。初始描述已经回答的内容
直接跳过；先只读核对源码，再用具体方案让用户回答“是”或“否 + 不同之处”。不要把通道、ROI、时间、
复位、上报、Web、验收等内部合同字段逐项询问。只有最初业务目标不清楚时才问一个开放问题，并给出简短
完整示例。第 1 个业务问题必须引导用户用 2–4 句话说明现场、关注对象、业务事件、系统结果、结果使用者
和有效标准，而不是只问“想看到什么结果”；不要在问题后罗列 GPIO、跨通道、周期上报等仓库能力菜单，
也不要提前索取模型、ROI 或协议细节。需求完整后读取必要的专项 `SKILL.md`，保留工作区已有改动，并按
照实际源码实施和验证。
"""
    if agent_key == "claude":
        return f"{prompt.rstrip()}\n\n{CLAUDE_TURN_PROTOCOL}\n"
    return prompt


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    entry_command = (
        "develop_feature.cmd" if platform.system().lower() == "windows"
        else "./develop_feature"
    )
    parser = argparse.ArgumentParser(
        description="用少量确认明确需求，并让 Codex 或 Claude Code 只生成通道/全局 Logic。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=f"""示例：
  {entry_command}
  {entry_command} --agent codex "新增人员越界告警并通过 HTTP 上报图片"
  {entry_command} --agent claude --confirm-before-code "开发跨通道人数聚合"
  {entry_command} --agent claude --plan-only "评估新增模型类型需要改哪些模块"
""",
    )
    parser.add_argument("description", nargs="*", help="可选的初始需求描述")
    parser.add_argument(
        "--agent",
        "--provider",
        dest="agent",
        choices=AGENT_CHOICES,
        default="auto",
        help="编程代理：auto（默认）、codex 或 claude",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--confirm-before-code",
        action="store_true",
        help="需求合同完成后再等待一次明确确认",
    )
    mode.add_argument(
        "--plan-only",
        action="store_true",
        help="只澄清和规划，不允许编程代理修改文件",
    )
    parser.add_argument("--model", help="传给所选编程代理的模型名称")
    parser.add_argument("--profile", help="Codex 专用：配置 profile")
    parser.add_argument(
        "--no-alt-screen",
        action="store_true",
        help="Codex 专用：保留终端滚动记录",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="只检查仓库、Skill、Codex 和 Claude Code 可用性",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="打印将交给所选代理的启动提示，不启动代理",
    )
    return parser.parse_args(argv)


def probe_agents(discovered: dict[str, str | None]) -> dict[str, AgentProbe]:
    probes = {}
    for key, definition in AGENT_DEFINITIONS.items():
        executable = discovered[key]
        if executable is None:
            probes[key] = AgentProbe(key, None, None, "未在 PATH 中找到")
            continue
        try:
            runtime = inspect_builtin_agent(key, executable)
        except AdapterError as exc:
            probes[key] = AgentProbe(key, executable, None, str(exc))
        else:
            probes[key] = AgentProbe(key, executable, runtime, None)
    return probes


def choose_agent(
    requested: str,
    probes: dict[str, AgentProbe],
    input_fn: Callable[[str], str] = input,
) -> AgentRuntime:
    if requested != "auto":
        probe = probes[requested]
        if not probe.usable:
            raise AdapterError(
                f"无法使用 {AGENT_DEFINITIONS[requested].label}：{probe.error}"
            )
        assert probe.runtime is not None
        return probe.runtime

    usable = [probes[key].runtime for key in AGENT_DEFINITIONS if probes[key].usable]
    runtimes = [runtime for runtime in usable if runtime is not None]
    if not runtimes:
        detail = "；".join(
            f"{AGENT_DEFINITIONS[key].label}: {probes[key].error}"
            for key in AGENT_DEFINITIONS
        )
        raise AdapterError(f"没有可用的 Codex CLI 或 Claude Code。{detail}")
    if len(runtimes) == 1:
        return runtimes[0]

    print("检测到多个可用编程代理：")
    for index, runtime in enumerate(runtimes, start=1):
        suffix = "（默认）" if index == 1 else ""
        print(f"  {index}. {runtime.display}{suffix}")
    while True:
        answer = input_fn(f"请选择本次使用的代理 [1-{len(runtimes)}，默认 1]：").strip()
        if not answer:
            return runtimes[0]
        if answer.isdigit() and 1 <= int(answer) <= len(runtimes):
            return runtimes[int(answer) - 1]
        print("输入无效，请输入列表中的数字。")


def effective_requested_agent(args: argparse.Namespace) -> str:
    if args.agent == "auto" and (args.profile or args.no_alt_screen):
        return "codex"
    return args.agent


def validate_cli_options(args: argparse.Namespace) -> None:
    if args.agent == "claude" and (args.profile or args.no_alt_screen):
        options = []
        if args.profile:
            options.append("--profile")
        if args.no_alt_screen:
            options.append("--no-alt-screen")
        raise AdapterError(
            f"{', '.join(options)} 只适用于 Codex，不能与 --agent claude 同时使用。"
        )


def print_preflight(
    repo: Path,
    host: HostEnvironment,
    probes: dict[str, AgentProbe],
) -> None:
    print(f"项目：{repo}")
    print(f"总入口：{repo / SKILL_RELATIVE}")
    print(f"专项 Skill：{len(REQUIRED_SKILLS) - 1} 个，检查通过")
    print("\n编程代理适配器：")
    for key, definition in AGENT_DEFINITIONS.items():
        probe = probes[key]
        if probe.runtime:
            print(f"- {definition.label}：可用，{probe.runtime.display}")
        elif probe.executable:
            print(f"- {definition.label}：不兼容，{probe.error}")
        else:
            print(f"- {definition.label}：未安装或不在 PATH")
    print("\n开发宿主自动检测：")
    print(host.report_block())
    print("\nLogic 写回保护：")
    print(f"- 白名单：{allowed_roots_text()}")
    print("- 无 Git 文件快照、哈希差异扫描、受控回写和越界整批拒绝：检查通过")
    print("- Codex/Claude：白名单内自动执行，不要求手动切换最高权限")
    if probes["claude"].usable:
        if claude_bash_sandbox_ready(host.kind):
            print("- Claude 写入型 Bash：强制沙箱可用")
        else:
            print("- Claude 写入型 Bash：当前宿主缺少受支持沙箱，自动禁用；文件编辑仍限 Logic 白名单")


def dry_run_agent_label(requested: str) -> str:
    if requested == "auto":
        return "auto（dry-run 未选择 Codex/Claude）"
    return f"{AGENT_DEFINITIONS[requested].label}（dry-run 未检查版本）"


def verify_write_guard(repo: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="rk3588-guard-self-test-") as holder:
        source = Path(holder) / "source"
        allowed = source / "vision_analysis/src/logic/modules/existing/logic.cpp"
        outside = source / "web_console/app.ts"
        excluded = source / "node_modules/cached/package.js"
        allowed.parent.mkdir(parents=True)
        outside.parent.mkdir(parents=True)
        excluded.parent.mkdir(parents=True)
        allowed.write_text("baseline\n", encoding="utf-8")
        outside.write_text("web-baseline\n", encoding="utf-8")
        excluded.write_text("cache-baseline\n", encoding="utf-8")

        with IsolatedLogicWorkspace(source) as isolated:
            assert isolated.path is not None
            if (isolated.path / "node_modules").exists():
                raise WriteBoundaryError("隔离副本错误复制了排除的依赖缓存。")
            generated = (
                isolated.path
                / "vision_analysis/src/logic/modules/generated/logic.cpp"
            )
            generated.parent.mkdir(parents=True)
            generated.write_text("generated\n", encoding="utf-8")
            promoted = isolated.promote()
            expected = "vision_analysis/src/logic/modules/generated/logic.cpp"
            if promoted.changed_paths != (expected,):
                raise WriteBoundaryError("允许路径回写自检结果不一致。")
        if not (source / expected).is_file():
            raise WriteBoundaryError("允许路径文件未能回写临时测试项目。")

        with IsolatedLogicWorkspace(source) as isolated:
            assert isolated.path is not None
            (isolated.path / expected).unlink()
            promoted = isolated.promote()
            if promoted.changed_paths != (expected,):
                raise WriteBoundaryError("允许路径删除自检结果不一致。")
        if (source / expected).exists():
            raise WriteBoundaryError("允许路径删除未能回写临时测试项目。")

        with IsolatedLogicWorkspace(source) as isolated:
            assert isolated.path is not None
            isolated_allowed = (
                isolated.path
                / "vision_analysis/src/logic/modules/existing/logic.cpp"
            )
            isolated_outside = isolated.path / "web_console/app.ts"
            isolated_allowed.write_text("must-not-land\n", encoding="utf-8")
            isolated_outside.write_text("must-not-land\n", encoding="utf-8")
            try:
                isolated.promote()
            except WriteBoundaryError as exc:
                if "web_console/app.ts" not in str(exc):
                    raise
            else:
                raise WriteBoundaryError("越界整批拒绝自检失败。")
        if allowed.read_text(encoding="utf-8") != "baseline\n":
            raise WriteBoundaryError("越界批次中的允许路径被错误回写。")

        with IsolatedLogicWorkspace(source) as isolated:
            assert isolated.path is not None
            isolated_allowed = (
                isolated.path
                / "vision_analysis/src/logic/modules/existing/logic.cpp"
            )
            isolated_allowed.write_text("agent-change\n", encoding="utf-8")
            allowed.write_text("concurrent-change\n", encoding="utf-8")
            try:
                isolated.promote()
            except WriteBoundaryError as exc:
                if "其他进程修改" not in str(exc):
                    raise
            else:
                raise WriteBoundaryError("原项目并发修改保护自检失败。")
        if allowed.read_text(encoding="utf-8") != "concurrent-change\n":
            raise WriteBoundaryError("并发修改内容被错误覆盖。")

    with IsolatedLogicWorkspace(repo) as isolated:
        if isolated.promote().changed_paths:
            raise WriteBoundaryError("无改动隔离副本产生了意外补丁。")
        assert isolated.path is not None
        probe = isolated.path / ".rk3588-write-boundary-probe"
        probe.write_text("must never reach the source repository\n", encoding="utf-8")
        try:
            isolated.promote()
        except WriteBoundaryError as exc:
            if probe.name not in str(exc):
                raise
            return
        raise WriteBoundaryError("越界写入探针未被拦截。")


def validate_isolated_catalog(workspace: Path, environment: dict[str, str]) -> None:
    result = subprocess.run(
        (
            sys.executable,
            "-I",
            "-B",
            "scripts/generate_logics_catalog.py",
            "--check",
        ),
        cwd=workspace / "vision_analysis",
        env=environment,
        check=False,
        capture_output=True,
    )
    stdout = decode_process_output(result.stdout)
    stderr = decode_process_output(result.stderr)
    if result.returncode != 0:
        detail = stderr.strip() or stdout.strip() or "未知错误"
        raise WriteBoundaryError(f"Logic manifest 校验失败：\n{detail}")
    success_output = stdout.strip() or stderr.strip()
    if success_output:
        print(success_output)


def _parse_claude_turn(output: str) -> tuple[str, str]:
    """Parse one Claude JSON result and return its guarded conversation state."""
    try:
        payload = json.loads(output)
    except json.JSONDecodeError as exc:
        raise AdapterError("Claude Code 没有返回有效的 JSON 结果。") from exc
    if not isinstance(payload, dict):
        raise AdapterError("Claude Code 返回的 JSON 顶层不是对象。")
    structured = payload.get("structured_output")
    if not isinstance(structured, dict):
        raise AdapterError("Claude Code 结果缺少 structured_output。")
    state = structured.get("state")
    message = structured.get("message")
    if state not in {"wait", "done"} or not isinstance(message, str):
        raise AdapterError("Claude Code 返回了不符合向导协议的结构化结果。")
    message = message.strip()
    if not message:
        raise AdapterError("Claude Code 返回了空消息。")
    return state, message


def run_claude_guided_session(
    runtime: AgentRuntime,
    workspace: Path,
    prompt: str,
    mode: str,
    environment: dict[str, str],
    host_kind: str,
    *,
    model: str | None = None,
    input_fn: Callable[[str], str] = input,
    max_turns: int = 12,
) -> int:
    """Relay a resumable Claude print-mode session without trust/permission dialogs."""
    session_id = str(uuid.uuid4())
    turn_prompt = prompt

    for turn_number in range(1, max_turns + 1):
        command = build_agent_command(
            runtime,
            workspace,
            turn_prompt,
            mode,
            model=model,
            host_kind=host_kind,
            claude_session_id=session_id,
            claude_resume=turn_number > 1,
            claude_json_schema=CLAUDE_TURN_SCHEMA,
        )
        print(f"Claude 正在处理第 {turn_number} 轮……", flush=True)
        result = subprocess.run(
            command,
            cwd=workspace,
            env=environment,
            check=False,
            capture_output=True,
        )
        stdout = decode_process_output(result.stdout)
        stderr = decode_process_output(result.stderr)
        if result.returncode != 0:
            detail = stderr.strip() or stdout.strip() or "未知错误"
            print(f"Claude Code 运行失败：{detail}", file=sys.stderr)
            return result.returncode or 2
        if stderr.strip():
            print(stderr.strip(), file=sys.stderr)
        try:
            state, message = _parse_claude_turn(stdout)
        except AdapterError as exc:
            print(f"Claude Code 结果无效：{exc}", file=sys.stderr)
            return 2

        print(f"\nClaude：\n{message}\n")
        if state == "done":
            return 0

        try:
            answer = input_fn("你的回答（输入 /exit 取消）：").strip()
        except EOFError:
            print("未收到用户回答，已取消；隔离副本不会写回。", file=sys.stderr)
            return 130
        if answer.lower() in {"/exit", "/quit"}:
            print("已取消 Claude 会话；隔离副本不会写回。")
            return 130
        if not answer:
            answer = "（用户未补充内容，请把上一题缩短后再问一次。）"
        turn_prompt = (
            "用户对你上一轮问题的回答如下：\n"
            f"{answer}\n\n"
            "请继续执行既定 RK3588 Logic-only 向导。仍需信息时返回 state=wait；"
            "完成实现、计划或边界阻塞说明时返回 state=done。"
        )

    print(
        f"Claude 会话超过 {max_turns} 轮安全上限；隔离副本不会写回。",
        file=sys.stderr,
    )
    return 2


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        validate_cli_options(args)
    except AdapterError as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 2
    try:
        repo = repository_root(Path(__file__).parent)
    except RuntimeError as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 2

    missing = missing_skill_files(repo)
    if missing:
        for path in missing:
            print(f"错误：缺少 Skill：{path}", file=sys.stderr)
        return 2

    host = detect_host_environment()
    mode = "plan-only" if args.plan_only else (
        "confirm" if args.confirm_before_code else "auto"
    )
    description = " ".join(args.description).strip()
    requested = effective_requested_agent(args)

    if args.dry_run:
        label = dry_run_agent_label(requested)
        prompt = build_prompt(
            description,
            mode,
            host,
            label,
            "dry-run 未创建实际权限边界",
            requested if requested in AGENT_DEFINITIONS else None,
        )
        print(prompt)
        return 0

    discovered = discover_builtin_agents()
    probes = probe_agents(discovered)

    if args.check:
        try:
            verify_write_guard(repo)
        except WriteBoundaryError as exc:
            print(f"错误：Logic 写回保护检查失败：{exc}", file=sys.stderr)
            return 2
        print_preflight(repo, host, probes)
        if requested == "auto":
            return 0 if any(probe.usable for probe in probes.values()) else 2
        return 0 if probes[requested].usable else 2

    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print("错误：开发向导需要交互式终端；检查配置可使用 --check。", file=sys.stderr)
        return 2

    print("\n开发宿主自动检测：")
    print(host.report_block())
    print("\n选定代理后，向导第一题只需用“是”或“否”确认上述结果与默认 RK3588 部署目标。\n")

    try:
        runtime = choose_agent(requested, probes)
        agent_permission = permission_summary(runtime.key, mode, host.kind)
        prompt = build_prompt(
            description,
            mode,
            host,
            runtime.display,
            agent_permission,
            runtime.key,
        )
    except AdapterError as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130

    print(f"\n本次编程代理：{runtime.display}")
    print(f"权限边界：{agent_permission}")
    print(f"原仓库写回白名单：{allowed_roots_text()}")
    print("正在创建一次性隔离工作副本；代理不会直接在原仓库中开发。\n")

    try:
        with IsolatedLogicWorkspace(repo) as isolated:
            assert isolated.path is not None
            child_environment = dict(isolated.child_environment())
            if runtime.key == "claude":
                returncode = run_claude_guided_session(
                    runtime,
                    isolated.path,
                    prompt,
                    mode,
                    child_environment,
                    host.kind,
                    model=args.model,
                )
            else:
                command = build_agent_command(
                    runtime,
                    isolated.path,
                    prompt,
                    mode,
                    model=args.model,
                    profile=args.profile,
                    no_alt_screen=args.no_alt_screen,
                    host_kind=host.kind,
                )
                result = subprocess.run(
                    command,
                    cwd=isolated.path,
                    env=child_environment,
                    check=False,
                )
                returncode = result.returncode
            if returncode != 0:
                print("代理未正常完成；隔离副本中的全部改动已丢弃。", file=sys.stderr)
                return returncode
            if mode == "plan-only":
                print("plan-only 会话结束；隔离副本已丢弃，原仓库没有写入。")
                return 0
            validate_isolated_catalog(isolated.path, child_environment)
            promotion = isolated.promote()
            if not promotion.changed_paths:
                print("会话未产生可回写的 Logic 文件；原仓库没有写入。")
                return 0
            print("已通过白名单检查并回写：")
            for path in promotion.changed_paths:
                print(f"- {path}")
            return 0
    except (OSError, WriteBoundaryError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
