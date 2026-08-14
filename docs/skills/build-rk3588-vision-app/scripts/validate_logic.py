#!/usr/bin/env python3
"""Statically validate one RK3588 channel or global logic module."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


REPO_MARKERS = (
    "vision_analysis/src/logic",
    "vision_analysis/scripts/generate_logics_catalog.py",
    "docs/skills",
)
BLOCKING_PATTERNS = {
    r"\b(?:sleep|usleep)\s*\(": "sleep in a real-time logic path",
    r"\b(?:system|popen)\s*\(": "subprocess execution in a real-time logic path",
    r"\b(?:curl_easy_perform|redisCommand)\b": "network call in a real-time logic path",
    r"\bcv::imwrite\s*\(": "image encoding/disk write in a real-time logic path",
    r"\b(?:fopen|ofstream)\b": "direct file I/O; verify it is intentionally bounded",
}
CODE_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp"}


def find_repo(start: Path) -> Path:
    for candidate in (start.resolve(), *start.resolve().parents):
        if all((candidate / marker).exists() for marker in REPO_MARKERS):
            return candidate
    raise SystemExit("error: cannot locate repository root; pass --repo")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", help="registration name, e.g. logic_helmet or global_demo")
    parser.add_argument("--repo", type=Path, help="repository root (auto-detected by default)")
    return parser.parse_args()


def string_set(items: object, key: str) -> set[str]:
    if not isinstance(items, list):
        return set()
    return {item[key] for item in items if isinstance(item, dict) and isinstance(item.get(key), str)}


def main() -> int:
    args = parse_args()
    repo = args.repo.resolve() if args.repo else find_repo(Path.cwd())
    is_global = args.module.startswith("global_")
    if not is_global and not args.module.startswith("logic_"):
        raise SystemExit("error: module must start with logic_ or global_")
    parent = repo / "vision_analysis/src/logic" / ("global_modules" if is_global else "modules")
    target = parent / args.module
    entry_path, json_path = target / "logic.cpp", target / "logic.json"
    errors: list[str] = []
    warnings: list[str] = []
    for path in (entry_path, json_path):
        if not path.is_file():
            errors.append(f"missing {path.relative_to(repo)}")
    if errors:
        for item in errors:
            print(f"ERROR: {item}")
        return 1

    source_files = sorted(
        path for path in target.rglob("*") if path.is_file() and path.suffix.lower() in CODE_SUFFIXES
    )
    cpp = "\n".join(path.read_text(encoding="utf-8") for path in source_files)
    try:
        manifest = json.loads(json_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: invalid {json_path.relative_to(repo)}: {exc}")
        return 1

    macro = "REGISTER_GLOBAL_LOGIC" if is_global else "REGISTER_LOGIC"
    registrations = re.findall(rf"\b{macro}\s*\(\s*([A-Za-z_]\w*)\s*\)", cpp)
    if registrations != [args.module]:
        errors.append(f"expected exactly {macro}({args.module}); found {registrations or 'none'}")
    if "name" in manifest:
        errors.append("source logic.json must not contain generated top-level key 'name'")
    if not isinstance(manifest.get("label"), str) or not manifest["label"].strip():
        errors.append("logic.json requires a non-empty string label")
    parameters = manifest.get("parameters")
    if not isinstance(parameters, dict) or parameters.get("type") != "object":
        errors.append("parameters must be a JSON Schema object")
    else:
        if parameters.get("additionalProperties") is not False:
            errors.append("parameters.additionalProperties must be false")
        if not isinstance(parameters.get("properties"), dict):
            errors.append("parameters.properties must be an object")

    if not isinstance(manifest.get("event_types"), list):
        errors.append("event_types is required and must be an array; use [] for no events")
    declared_events = string_set(manifest.get("event_types"), "id")
    used_events = set(re.findall(r'\.event_type\s*=\s*"([^"\n]+)"', cpp))
    if used_events - declared_events:
        errors.append(f"undeclared event type(s): {sorted(used_events - declared_events)}")
    if "report_event(" in cpp and not declared_events:
        errors.append("module calls report_event() but declares no event_types")

    declared_fields = string_set(manifest.get("report_fields"), "key")
    used_fields = set(re.findall(r'\bevent_(?:json_)?field\s*\(\s*"([^"\n]+)"', cpp))
    used_fields.update(
        re.findall(r'\.fields\.set_(?:string|number|bool|json)\s*\(\s*"([^"\n]+)"', cpp)
    )
    if used_fields - declared_fields:
        errors.append(f"undeclared report field(s): {sorted(used_fields - declared_fields)}")
    declared_field_types = {
        item["key"]: item.get("type")
        for item in (manifest.get("report_fields") or [])
        if isinstance(item, dict) and isinstance(item.get("key"), str)
    }
    setter_types = {"string": "string", "number": "number", "bool": "boolean", "json": "json"}
    for setter, key in re.findall(
        r'\.fields\.set_(string|number|bool|json)\s*\(\s*"([^"\n]+)"', cpp
    ):
        declared_type = declared_field_types.get(key)
        if declared_type is not None and declared_type != setter_types[setter]:
            errors.append(
                f"report field {key!r} uses set_{setter} but declares type {declared_type!r}"
            )
    unused_fields = declared_fields - used_fields
    if unused_fields:
        warnings.append(f"report_fields not found as C++ literals: {sorted(unused_fields)}")

    declared_params = set(parameters.get("properties", {})) if isinstance(parameters, dict) else set()
    used_params = set(re.findall(r'\bparam_(?:float|int|bool|string|json)\s*\(\s*"([^"\n]+)"', cpp))
    if used_params - declared_params:
        errors.append(f"undeclared logic parameter(s): {sorted(used_params - declared_params)}")
    declared_actions = string_set(manifest.get("actions"), "id")
    has_action_registration = "REGISTER_LOGIC_ACTION" in cpp
    if is_global and declared_actions:
        errors.append("global logic actions are not supported by the current control path")
    elif declared_actions and not has_action_registration:
        errors.append("logic.json declares actions but C++ has no REGISTER_LOGIC_ACTION")
    for pattern, message in BLOCKING_PATTERNS.items():
        if re.search(pattern, cpp):
            warnings.append(message)

    catalog = repo / "vision_analysis/scripts/generate_logics_catalog.py"
    result = subprocess.run(
        [sys.executable, str(catalog), "--check"],
        cwd=repo / "vision_analysis",
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode:
        details = (result.stdout + result.stderr).strip().replace("\n", " | ")
        errors.append(f"catalog validation failed: {details}")

    for item in warnings:
        print(f"WARNING: {item}")
    for item in errors:
        print(f"ERROR: {item}")
    if errors:
        return 1
    print(f"OK: {args.module} passed static validation ({len(warnings)} warning(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
