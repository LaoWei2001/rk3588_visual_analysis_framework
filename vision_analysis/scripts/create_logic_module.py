#!/usr/bin/env python3
"""Create a channel or global logic module with the same manifest contract."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
from typing import Iterable, Optional


IDENTIFIER_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def channel_source(name: str) -> str:
    return f'''#include "logic/core/logic_common.h"

namespace
{{

static void {name}(ChannelContext *ctx)
{{
    if (!ctx)
        return;

    // 在这里编写逐帧业务逻辑；事件上报使用框架提供的 EventRequest 接口。
}}

}} // namespace

REGISTER_LOGIC({name});
'''


def global_source(name: str) -> str:
    return f'''#include "logic/core/global_logic.h"

namespace
{{

static void {name}(GlobalContext *gctx)
{{
    if (!gctx)
        return;

    for (const ChannelInput &channel : gctx->inputs())
    {{
        // 这里只处理有效业务输入；离线、未发布和过期通道已由框架排除。
        (void)channel;
    }}
}}

}} // namespace

REGISTER_GLOBAL_LOGIC({name});
'''


def manifest(label: str, kind: str) -> dict:
    value = {
        "label": label,
        "report_templates": [],
        "event_types": [],
        "parameters": {
            "type": "object",
            "additionalProperties": False,
            "properties": {},
        },
        "report_fields": [],
        "actions": [],
        "business_fields": [],
    }
    if kind == "channel":
        value["outputs"] = []
    return value


def parse_args(argv: Optional[Iterable[str]] = None) -> argparse.Namespace:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("channel", "global"))
    parser.add_argument("name", help="C++ function name and unique logic ID")
    parser.add_argument("--label", help="Web display label; defaults to name")
    parser.add_argument(
        "--project-root",
        type=Path,
        default=project_root,
        help="vision_analysis project root",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = parse_args(argv)
    if not IDENTIFIER_RE.fullmatch(args.name):
        raise SystemExit(f"invalid logic name: {args.name}")

    collection = "modules" if args.kind == "channel" else "global_modules"
    module_dir = args.project_root.resolve() / "src" / "logic" / collection / args.name
    if module_dir.exists():
        raise SystemExit(f"module already exists: {module_dir}")
    module_dir.mkdir(parents=True)

    source = channel_source(args.name) if args.kind == "channel" else global_source(args.name)
    (module_dir / "logic.cpp").write_text(source, encoding="utf-8")
    (module_dir / "logic.json").write_text(
        json.dumps(manifest(args.label or args.name, args.kind), ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(module_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
