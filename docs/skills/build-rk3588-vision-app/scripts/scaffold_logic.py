#!/usr/bin/env python3
"""Create a minimal channel/global logic module without overwriting files."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


REPO_MARKERS = (
    "vision_analysis/src/logic",
    "vision_analysis/scripts/generate_logics_catalog.py",
    "docs/skills",
)


def find_repo(start: Path) -> Path:
    for candidate in (start.resolve(), *start.resolve().parents):
        if all((candidate / marker).exists() for marker in REPO_MARKERS):
            return candidate
    raise SystemExit("error: cannot locate repository root from current directory; pass --repo")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, help="repository root (auto-detected by default)")
    parser.add_argument("--kind", choices=("channel", "global"), required=True)
    parser.add_argument("--name", required=True, help="logic_xxx or global_xxx registration name")
    parser.add_argument("--label", required=True, help="human-readable module label")
    parser.add_argument("--dry-run", action="store_true", help="show files without writing")
    return parser.parse_args()


def module_content(kind: str, name: str, label: str) -> tuple[str, str]:
    if kind == "channel":
        cpp = f'''#include "logic/core/logic_common.h"

static void {name}(ChannelContext *ctx)
{{
    if (!ctx) return;
    // TODO: implement the requirement contract. Keep blocking I/O out of this path.
}}

REGISTER_LOGIC({name});
'''
        manifest = {
            "label": label,
            "event_types": [],
            "parameters": {
                "type": "object",
                "additionalProperties": False,
                "properties": {},
            },
            "report_fields": [],
        }
    else:
        cpp = f'''#include "logic/core/global_logic.h"

static void {name}(GlobalContext *gctx)
{{
    if (!gctx) return;
    // TODO: aggregate channel snapshots/outputs. Keep blocking I/O out of this path.
}}

REGISTER_GLOBAL_LOGIC({name});
'''
        manifest = {
            "label": label,
            "event_types": [],
            "report_fields": [],
            "business_fields": [],
            "parameters": {
                "type": "object",
                "additionalProperties": False,
                "properties": {},
            },
        }
    return cpp, json.dumps(manifest, ensure_ascii=False, indent=4) + "\n"


def main() -> int:
    args = parse_args()
    repo = args.repo.resolve() if args.repo else find_repo(Path.cwd())
    expected_prefix = "logic_" if args.kind == "channel" else "global_"
    if not re.fullmatch(r"[a-z][a-z0-9_]*", args.name) or not args.name.startswith(expected_prefix):
        raise SystemExit(f"error: {args.kind} name must match {expected_prefix}[a-z0-9_]+")
    if not args.label.strip():
        raise SystemExit("error: label must not be empty")

    parent = repo / "vision_analysis/src/logic" / (
        "modules" if args.kind == "channel" else "global_modules"
    )
    if not parent.is_dir():
        raise SystemExit(f"error: module parent does not exist: {parent}")
    target = parent / args.name
    if target.exists():
        raise SystemExit(f"error: refusing to overwrite existing path: {target}")

    cpp, manifest = module_content(args.kind, args.name, args.label.strip())
    print(f"module: {target.relative_to(repo)}")
    print("files: logic.cpp, logic.json")
    if args.dry_run:
        return 0

    target.mkdir()
    (target / "logic.cpp").write_text(cpp, encoding="utf-8")
    (target / "logic.json").write_text(manifest, encoding="utf-8")
    print("created; next run generate_logics_catalog.py --check and validate_logic.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
