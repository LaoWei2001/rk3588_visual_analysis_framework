#!/usr/bin/env python3
"""Check documentation links, source entry points and known stale claims."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from urllib.parse import unquote


REPO_MARKERS = ("vision_analysis/src", "docs/skills")
REQUIRED_PATHS = (
    "docs/README.md",
    "docs/skills/README.md",
    "docs/skills/rk3588-channel-logic/SKILL.md",
    "docs/skills/rk3588-global-logic/SKILL.md",
    "docs/skills/rk3588-console-ops/SKILL.md",
    "vision_analysis/src/logic/core/channel_logic.h",
    "vision_analysis/src/logic/core/global_logic.h",
    "vision_analysis/src/event/event_report.h",
)
STALE_PATTERNS = {
    r"里面有\*\*三个 Skill\*\*|先分清三个 Skill": "skill count predates build-rk3588-vision-app",
    r"运行时\(8 类线程": "runtime thread count predates the current main.cpp inventory",
    r"`ctx->roi`": "ChannelContext no longer exposes the singular roi member",
    r"tests/test_event_report|event_report_unit_test|test_event_store\.py": "referenced test path does not exist",
    r"`logic_course_08` ～ `logic_course_10` 当前仍": "course_08 is implemented; only 09-10 are task skeletons",
    r"不保留旧上报接口或旧配置的兼容分支": "the outbox intentionally resumes legacy 12-attempt failures",
    r"alarm_image_worker": "the image worker is named image_worker in current event_report.cpp",
}
LINK_RE = re.compile(r"(?<!!)\[[^\]]*\]\(([^)]+)\)")
INLINE_REPO_PATH_RE = re.compile(r"`((?:vision_analysis|web_console|service|docs)/[^`\s]+)`")
FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})([^`]*)$")


def concrete_repo_paths(raw: str) -> list[str]:
    """Expand exact inline repo paths; return [] for documented placeholders/globs."""
    if any(char in raw for char in "<>{}*[]"):
        return []
    if any(token in raw for token in ("logic_xxx", "logic_foo", "global_total_person")):
        return []
    if raw.endswith(".h/.cpp"):
        stem = raw[: -len(".h/.cpp")]
        return [stem + ".h", stem + ".cpp"]
    return [raw]


def find_repo(start: Path) -> Path:
    for candidate in (start.resolve(), *start.resolve().parents):
        if all((candidate / marker).exists() for marker in REPO_MARKERS):
            return candidate
    raise SystemExit("error: cannot locate repository root; pass --repo")


def json_fences(text: str):
    """Yield (opening line, body) for fenced blocks explicitly marked json."""
    marker_char = ""
    marker_len = 0
    language = ""
    start_line = 0
    body: list[str] = []
    for lineno, line in enumerate(text.splitlines(), 1):
        match = FENCE_RE.match(line)
        if not marker_char:
            if not match:
                continue
            marker = match.group(1)
            marker_char = marker[0]
            marker_len = len(marker)
            language = match.group(2).strip().lower()
            start_line = lineno
            body = []
            continue
        stripped = line.strip()
        if stripped and set(stripped) == {marker_char} and len(stripped) >= marker_len:
            if language == "json":
                yield start_line, "\n".join(body)
            marker_char = ""
            marker_len = 0
            language = ""
            body = []
            continue
        body.append(line)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, help="repository root (auto-detected by default)")
    args = parser.parse_args()
    repo = args.repo.resolve() if args.repo else find_repo(Path.cwd())
    errors: list[str] = []
    for rel in REQUIRED_PATHS:
        if not (repo / rel).exists():
            errors.append(f"missing required entry point: {rel}")

    docs_root = repo / "docs"
    markdown_files = sorted(docs_root.rglob("*.md"))
    for path in markdown_files:
        text = path.read_text(encoding="utf-8")
        rel = path.relative_to(repo)
        in_fence = False
        visible_lines: list[str] = []
        for lineno, line in enumerate(text.splitlines(), 1):
            if line.lstrip().startswith(("```", "~~~")):
                in_fence = not in_fence
                visible_lines.append("")
                continue
            if in_fence:
                visible_lines.append("")
                continue
            visible_lines.append(line)
            for raw_target in LINK_RE.findall(line):
                target = raw_target.strip().split(maxsplit=1)[0].strip("<>")
                if not target or target.startswith(("#", "http://", "https://", "mailto:")):
                    continue
                file_part = unquote(target.split("#", 1)[0])
                if file_part and not (path.parent / file_part).resolve().exists():
                    errors.append(f"{rel}:{lineno}: broken relative link: {target}")
            for raw_path in INLINE_REPO_PATH_RE.findall(line):
                for repo_path in concrete_repo_paths(raw_path):
                    if not (repo / repo_path).exists():
                        errors.append(f"{rel}:{lineno}: missing inline repository path: {repo_path}")

        # development_log intentionally preserves historical snapshots.
        if path.name == "development_log.md":
            continue
        visible_text = "\n".join(visible_lines)
        for pattern, reason in STALE_PATTERNS.items():
            for match in re.finditer(pattern, visible_text):
                lineno = text.count("\n", 0, match.start()) + 1
                errors.append(f"{rel}:{lineno}: stale claim ({reason})")

        for lineno, body in json_fences(text):
            try:
                value = json.loads(body)
            except json.JSONDecodeError:
                # Many docs intentionally show JSON fragments. The catalog remains
                # authoritative for executable manifests.
                continue
            if (
                isinstance(value, dict)
                and isinstance(value.get("label"), str)
                and isinstance(value.get("parameters"), dict)
                and "event_types" not in value
            ):
                errors.append(
                    f"{rel}:{lineno}: complete logic.json example omits required event_types"
                )

    if errors:
        for item in errors:
            print(f"ERROR: {item}")
        print(f"FAILED: {len(errors)} documentation issue(s) in {len(markdown_files)} file(s)")
        return 1
    print(f"OK: {len(markdown_files)} Markdown file(s), links and stale-claim rules passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
