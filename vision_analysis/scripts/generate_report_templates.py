#!/usr/bin/env python3
"""Collect and validate application report-contract templates."""

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Tuple

from generate_logics_catalog import (
    ManifestError,
    build_catalog,
    resolved_report_template_id,
)


ALLOWED_MEDIA = {"annotated_image", "raw_image", "video"}
ALLOWED_LOCATIONS = {"body", "query", "form", "header", "file"}
ALLOWED_SOURCE_ROOTS = {"event", "source", "fields", "media"}
ALLOWED_TYPES = {"", "string", "number", "boolean", "json"}


def load_json(path: Path) -> Dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise ManifestError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ManifestError(f"{path}: template must be an object")
    return value


def template_paths(root: Path) -> List[Path]:
    return sorted(root.rglob("*.json")) if root.is_dir() else []


def validate_template(
    path: Path,
    contract: Dict[str, Any],
    logics: Dict[str, Dict[str, Any]],
    adapters: Dict[str, Dict[str, Any]],
) -> None:
    contract_id = contract.get("id")
    if not isinstance(contract_id, str) or not contract_id:
        raise ManifestError(f"{path}: id is required")
    if not isinstance(contract.get("version"), int) or contract["version"] < 1:
        raise ManifestError(f"{path}: version must be a positive integer")
    adapter_id = contract.get("adapter")
    adapter = adapters.get(adapter_id) if isinstance(adapter_id, str) else None
    if adapter is None:
        raise ManifestError(f"{path}: unknown adapter {adapter_id}")
    media = contract.get("media")
    if not isinstance(media, list) or any(item not in ALLOWED_MEDIA for item in media):
        raise ManifestError(f"{path}: media is invalid")
    if any(item not in adapter.get("supported_media", []) for item in media):
        raise ManifestError(f"{path}: adapter {adapter_id} does not support selected media")
    mappings = contract.get("mapping")
    if not isinstance(mappings, list) or not mappings:
        raise ManifestError(f"{path}: mapping must be a non-empty array")

    owner = contract.get("owner_logic")
    logic = logics.get(owner) if isinstance(owner, str) else None
    if owner is not None and logic is None:
        raise ManifestError(f"{path}: unknown owner_logic {owner}")
    declared_fields = {
        str(item.get("key")) for item in (logic or {}).get("report_fields", [])
        if isinstance(item, dict) and item.get("key")
    }
    declared_events = {
        str(item.get("id")) for item in (logic or {}).get("event_types", [])
        if isinstance(item, dict) and item.get("id")
    }
    event_types = contract.get("event_types", [])
    if not isinstance(event_types, list) or any(not isinstance(item, str) for item in event_types):
        raise ManifestError(f"{path}: event_types must be a string array")
    if logic is not None:
        if not event_types:
            raise ManifestError(f"{path}: logic-owned template requires event_types")
        unknown_events = set(event_types) - declared_events
        if unknown_events:
            raise ManifestError(f"{path}: undeclared event types: {', '.join(sorted(unknown_events))}")

    transforms = set(adapter.get("transforms", []))
    targets = set()
    adapter_targets = set()
    for index, mapping in enumerate(mappings):
        if not isinstance(mapping, dict):
            raise ManifestError(f"{path}: mapping[{index}] must be an object")
        source = mapping.get("source")
        target = mapping.get("target")
        location = mapping.get("location", "body")
        transform = mapping.get("transform", "")
        if not isinstance(source, str) or not source or not isinstance(target, str) or not target:
            raise ManifestError(f"{path}: mapping[{index}] requires source and target")
        if location not in ALLOWED_LOCATIONS:
            raise ManifestError(f"{path}: mapping[{index}] has invalid location {location}")
        if location != "body" and "." in target:
            raise ManifestError(f"{path}: non-body target cannot be nested: {target}")
        if transform not in transforms:
            raise ManifestError(f"{path}: mapping[{index}] has unsupported transform {transform}")
        if transform in ("base64", "data_url", "file") and not source.startswith("media."):
            raise ManifestError(f"{path}: file-content transform requires a media source")
        if location == "file" and transform != "file":
            raise ManifestError(f"{path}: file location requires transform=file")
        target_key = f"{location}:{target}"
        if target_key in targets:
            raise ManifestError(f"{path}: duplicate target {target_key}")
        targets.add(target_key)
        if adapter_id == "dify_workflow" and target in adapter_targets:
            raise ManifestError(f"{path}: duplicate Dify input {target}")
        adapter_targets.add(target)
        if adapter_id == "dify_workflow" and location not in ("body", "file"):
            raise ManifestError(f"{path}: Dify mappings only support body/file")
        if source == "constant":
            if "value" not in mapping:
                raise ManifestError(f"{path}: constant mapping[{index}] requires value")
        else:
            root = source.partition(".")[0]
            if root not in ALLOWED_SOURCE_ROOTS:
                raise ManifestError(f"{path}: mapping[{index}] has invalid source {source}")
        if source.startswith("fields.") and logic is not None:
            field = source.split(".", 1)[1]
            if field not in declared_fields:
                raise ManifestError(f"{path}: owner logic does not declare field {field}")
        if source.startswith("media.") and source.split(".", 1)[1] not in media:
            raise ManifestError(f"{path}: mapping source {source} is not enabled in media")
        if mapping.get("type", "") not in ALLOWED_TYPES:
            raise ManifestError(f"{path}: mapping[{index}] has invalid type")
        if transform == "file" and mapping.get("file_mode", "list") not in ("single", "list"):
            raise ManifestError(f"{path}: mapping[{index}] has invalid file_mode")

    if adapter_id == "http":
        request = contract.get("request")
        if not isinstance(request, dict):
            raise ManifestError(f"{path}: HTTP template requires request")
        if request.get("method") not in ("POST", "PUT", "PATCH"):
            raise ManifestError(f"{path}: invalid HTTP method")
        if not isinstance(request.get("path"), str) or not request["path"]:
            raise ManifestError(f"{path}: HTTP request.path is required")
        if request.get("encoding") not in ("json", "form", "multipart"):
            raise ManifestError(f"{path}: invalid HTTP encoding")
        locations = {mapping.get("location", "body") for mapping in mappings}
        if request["encoding"] == "json" and locations & {"form", "file"}:
            raise ManifestError(f"{path}: JSON request cannot contain form/file mappings")
        if request["encoding"] == "form" and locations & {"body", "file"}:
            raise ManifestError(f"{path}: form request only accepts form/query/header mappings")
        if request["encoding"] == "multipart" and "body" in locations:
            json_part = request.get("json_part")
            if not isinstance(json_part, str) or not json_part.strip():
                raise ManifestError(f"{path}: multipart body mappings require request.json_part")
            occupied = {
                str(mapping.get("target", "")) for mapping in mappings
                if mapping.get("location", "body") in ("form", "file")
            }
            if json_part in occupied:
                raise ManifestError(f"{path}: request.json_part conflicts with form/file target")
        if request["encoding"] == "multipart" and not locations & {"body", "file"}:
            raise ManifestError(f"{path}: multipart requires a file or JSON body part")


def atomic_write(path: Path, value: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--logic-root", type=Path, required=True)
    parser.add_argument("--app-dir", type=Path, required=True)
    parser.add_argument("--adapter-catalog", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    catalog = build_catalog(args.logic_root)
    logic_list = [*catalog["channel_logics"], *catalog["global_logics"]]
    logics = {str(item["name"]): item for item in logic_list}
    adapter_values = json.loads(args.adapter_catalog.read_text(encoding="utf-8"))
    adapters = {
        str(item["id"]): item for item in adapter_values
        if isinstance(item, dict) and item.get("id")
    }

    sources: List[Tuple[Path, Dict[str, Any]]] = []
    declared_module_paths = set()
    for logic in logic_list:
        module_group = "global_modules" if logic in catalog["global_logics"] else "modules"
        module_dir = args.logic_root / module_group / str(logic["name"])
        for relative in logic.get("report_templates", []):
            if not isinstance(relative, str):
                raise ManifestError(f"{module_dir}/logic.json: report_templates entries must be strings")
            path = (module_dir / relative).resolve()
            if module_dir.resolve() not in path.parents or not path.is_file():
                raise ManifestError(f"{module_dir}/logic.json: missing report template {relative}")
            declared_module_paths.add(path)
            contract = load_json(path)
            contract["id"] = resolved_report_template_id(path, contract, str(logic["name"]))
            sources.append((path, contract))
    all_module_paths = set(args.logic_root.glob("modules/*/report_templates/*.json")) | set(
        args.logic_root.glob("global_modules/*/report_templates/*.json")
    )
    undeclared = sorted(path for path in all_module_paths if path.resolve() not in declared_module_paths)
    if undeclared:
        raise ManifestError(f"undeclared module report template: {undeclared[0]}")

    for path in template_paths(args.app_dir):
        contract = load_json(path)
        owner_logic = str(contract.get("owner_logic", "")).strip()
        contract["id"] = resolved_report_template_id(path, contract, owner_logic)
        sources.append((path, contract))

    seen = {}
    for path, contract in sources:
        validate_template(path, contract, logics, adapters)
        contract_id = str(contract["id"])
        if contract_id in seen:
            raise ManifestError(f"duplicate contract id {contract_id}: {seen[contract_id]} and {path}")
        seen[contract_id] = path

    args.output.mkdir(parents=True, exist_ok=True)
    for old in args.output.glob("*.json"):
        old.unlink()
    for path, contract in sources:
        atomic_write(args.output / f"{contract['id']}.json", contract)
    print(f"Collected {len(sources)} report templates into {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ManifestError, OSError, ValueError) as exc:
        print(f"[ERROR] {exc}", file=sys.stderr)
        raise SystemExit(1)
