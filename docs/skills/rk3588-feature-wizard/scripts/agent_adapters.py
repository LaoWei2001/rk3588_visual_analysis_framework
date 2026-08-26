#!/usr/bin/env python3
"""Provider-specific command adapters for the RK3588 feature wizard."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
import shutil
import subprocess
from typing import Callable, Iterable, Sequence


SUPPORTED_AGENT_KEYS = ("codex", "claude")
AGENT_CHOICES = ("auto", *SUPPORTED_AGENT_KEYS)
PROBE_TIMEOUT_SECONDS = 15


class AdapterError(ValueError):
    """Raised when an agent cannot be resolved or safely configured."""


@dataclass(frozen=True)
class AgentDefinition:
    key: str
    label: str
    commands: tuple[str, ...]
    required_help_tokens: tuple[str, ...]


@dataclass(frozen=True)
class AgentRuntime:
    key: str
    label: str
    executable: str | None
    version: str

    @property
    def display(self) -> str:
        if self.executable:
            return f"{self.label} {self.version}（{self.executable}）"
        return f"{self.label}（{self.version}）"


AGENT_DEFINITIONS = {
    "codex": AgentDefinition(
        key="codex",
        label="Codex CLI",
        commands=("codex",),
        required_help_tokens=("--cd", "--sandbox", "--ask-for-approval"),
    ),
    "claude": AgentDefinition(
        key="claude",
        label="Claude Code",
        commands=("claude",),
        required_help_tokens=(
            "--permission-mode",
            "--allowedTools",
            "--tools",
            "--settings",
            "--setting-sources",
            "--strict-mcp-config",
            "--no-chrome",
            "--print",
            "--output-format",
            "--json-schema",
            "--session-id",
            "--resume",
        ),
    ),
}


CLAUDE_SANDBOX_HOSTS = {
    "macos",
    "windows-wsl2",
    "linux-rk3588",
    "linux-x86-64",
    "linux-arm64",
    "linux-other",
}
CLAUDE_LOGIC_EDIT_RULES = (
    "Edit(/vision_analysis/src/logic/modules/**)",
    "Edit(/vision_analysis/src/logic/global_modules/**)",
)
CODEX_ISOLATION_OVERRIDES = (
    "sandbox_workspace_write.writable_roots=[]",
    "sandbox_workspace_write.network_access=false",
    "sandbox_workspace_write.exclude_tmpdir_env_var=true",
    "sandbox_workspace_write.exclude_slash_tmp=true",
    "features.apps=false",
    "features.plugins=false",
    "features.remote_plugin=false",
    "features.skill_mcp_dependency_install=false",
    "features.hooks=false",
    "mcp_servers={}",
)


def claude_bash_sandbox_ready(
    host_kind: str | None,
    which: Callable[[str], str | None] = shutil.which,
) -> bool:
    """Return whether Claude can enforce no-prompt Bash isolation on this host."""
    if host_kind == "macos":
        return True
    if host_kind in CLAUDE_SANDBOX_HOSTS:
        return bool(which("bwrap") and which("socat"))
    return False


def find_executable(
    commands: Iterable[str],
    which: Callable[[str], str | None] = shutil.which,
) -> str | None:
    for command in commands:
        resolved = which(command)
        if resolved:
            return resolved
    return None


def discover_builtin_agents(
    which: Callable[[str], str | None] = shutil.which,
) -> dict[str, str | None]:
    """Return every built-in adapter and its resolved executable, if installed."""
    return {
        key: find_executable(definition.commands, which)
        for key, definition in AGENT_DEFINITIONS.items()
    }


def run_text_command(executable: str, args: Sequence[str]) -> tuple[int, str]:
    try:
        result = subprocess.run(
            [executable, *args],
            check=False,
            capture_output=True,
            text=True,
            timeout=PROBE_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise AdapterError(
            f"{executable} {' '.join(args)} 在 {PROBE_TIMEOUT_SECONDS} 秒内未完成。"
        ) from exc
    except OSError as exc:
        raise AdapterError(f"无法运行 {executable}：{exc}") from exc
    output = result.stdout.strip() or result.stderr.strip()
    return result.returncode, output


def inspect_builtin_agent(key: str, executable: str) -> AgentRuntime:
    definition = AGENT_DEFINITIONS[key]
    code, version_output = run_text_command(executable, ("--version",))
    if code != 0:
        raise AdapterError(
            f"{definition.label} --version 失败：{version_output or '未知错误'}"
        )

    code, help_output = run_text_command(executable, ("--help",))
    if code != 0:
        raise AdapterError(
            f"{definition.label} --help 失败：{help_output or '未知错误'}"
        )
    missing = [
        token for token in definition.required_help_tokens if token not in help_output
    ]
    if missing:
        raise AdapterError(
            f"{definition.label} 版本与向导适配器不兼容，缺少参数："
            + ", ".join(missing)
        )

    version = version_output.splitlines()[0] if version_output else "版本未知"
    return AgentRuntime(key, definition.label, executable, version)


def permission_summary(agent_key: str, mode: str, host_kind: str | None = None) -> str:
    if agent_key == "codex":
        if mode == "plan-only":
            return "Codex read-only 沙箱；never 审批策略，不弹出权限确认"
        return "Codex 隔离副本 workspace-write 沙箱；never 审批策略，不弹出权限确认"
    if agent_key == "claude":
        if mode == "plan-only":
            return "Claude Code dontAsk 只读工具白名单；不弹出权限确认"
        if claude_bash_sandbox_ready(host_kind):
            return "Claude Code dontAsk Logic 编辑白名单 + 强制 Bash 沙箱；不弹出权限确认"
        return "Claude Code dontAsk Logic 编辑白名单；当前宿主未启用写入型 Bash，不弹出权限确认"
    raise AdapterError(f"没有实现代理适配器：{agent_key}")


def claude_session_settings(host_kind: str | None) -> str:
    """Return isolated, non-interactive Claude Code settings for one session."""
    settings: dict[str, object] = {
        "permissions": {
            "defaultMode": "dontAsk",
            "disableBypassPermissionsMode": "disable",
        }
    }
    if claude_bash_sandbox_ready(host_kind):
        settings["sandbox"] = {
            "enabled": True,
            "failIfUnavailable": True,
            "autoAllowBashIfSandboxed": True,
            "allowUnsandboxedCommands": False,
        }
    return json.dumps(settings, ensure_ascii=False, separators=(",", ":"))


def _reject_unsupported_options(
    agent_key: str,
    profile: str | None,
    no_alt_screen: bool,
) -> None:
    unsupported = []
    if profile:
        unsupported.append("--profile")
    if no_alt_screen:
        unsupported.append("--no-alt-screen")
    if unsupported:
        raise AdapterError(
            f"{', '.join(unsupported)} 只适用于 Codex；当前代理是 {agent_key}。"
        )


def build_agent_command(
    runtime: AgentRuntime,
    repo: Path,
    prompt: str,
    mode: str,
    *,
    model: str | None = None,
    profile: str | None = None,
    no_alt_screen: bool = False,
    host_kind: str | None = None,
    claude_session_id: str | None = None,
    claude_resume: bool = False,
    claude_json_schema: str | None = None,
) -> list[str]:
    """Build an argv list without invoking a shell."""
    if mode not in {"auto", "confirm", "plan-only"}:
        raise AdapterError(f"未知运行模式：{mode}")
    if runtime.executable is None:
        raise AdapterError(f"{runtime.label} 没有可执行程序。")

    if runtime.key == "codex":
        if claude_session_id or claude_resume or claude_json_schema:
            raise AdapterError("Claude 多轮参数不能用于 Codex。")
        command = [
            runtime.executable,
            "--cd",
            str(repo),
            "--sandbox",
            "read-only" if mode == "plan-only" else "workspace-write",
            "--ask-for-approval",
            "never",
        ]
        if model:
            command.extend(("--model", model))
        if profile:
            command.extend(("--profile", profile))
        for override in CODEX_ISOLATION_OVERRIDES:
            command.extend(("--config", override))
        if no_alt_screen:
            command.append("--no-alt-screen")
        command.append(prompt)
        return command

    _reject_unsupported_options(runtime.key, profile, no_alt_screen)

    if runtime.key == "claude":
        if claude_resume and not claude_session_id:
            raise AdapterError("Claude --resume 缺少 session id。")
        if claude_session_id and not claude_json_schema:
            raise AdapterError("Claude 结构化多轮会话缺少 JSON Schema。")
        available_tools = ["Read", "Glob", "Grep"]
        allowed_tools = ["Read", "Glob", "Grep"]
        if mode != "plan-only":
            # Claude applies Edit(path) rules to both Edit and Write file tools.
            available_tools.extend(("Edit", "Write"))
            allowed_tools.extend(CLAUDE_LOGIC_EDIT_RULES)
            if claude_bash_sandbox_ready(host_kind):
                available_tools.append("Bash")
                allowed_tools.append("Bash")
        command = [
            runtime.executable,
            "--permission-mode",
            "dontAsk",
            "--allowedTools",
            ",".join(allowed_tools),
            "--tools",
            ",".join(available_tools),
            "--setting-sources",
            "",
            "--settings",
            claude_session_settings(host_kind),
            "--strict-mcp-config",
            "--no-chrome",
        ]
        if claude_session_id:
            command.extend(("--print", "--output-format", "json"))
            command.extend(("--json-schema", claude_json_schema))
            if claude_resume:
                command.extend(("--resume", claude_session_id))
            else:
                command.extend(("--session-id", claude_session_id))
        if model:
            command.extend(("--model", model))
        command.append(prompt)
        return command

    raise AdapterError(f"没有实现代理适配器：{runtime.key}")
