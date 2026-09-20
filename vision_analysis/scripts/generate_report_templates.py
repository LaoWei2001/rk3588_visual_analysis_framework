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
    except json.JSONDecodeError as exc:
        raise ManifestError(
            f"{path}：JSON 格式错误（第 {exc.lineno} 行，第 {exc.colno} 列）"
        ) from exc
    except OSError as exc:
        raise ManifestError(f"{path}：无法读取 JSON 文件") from exc
    if not isinstance(value, dict):
        raise ManifestError(f"{path}：模板必须是对象")
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
        raise ManifestError(f"{path}：id 为必填项")
    if not isinstance(contract.get("version"), int) or contract["version"] < 1:
        raise ManifestError(f"{path}：version 必须是正整数")
    adapter_id = contract.get("adapter")
    adapter = adapters.get(adapter_id) if isinstance(adapter_id, str) else None
    if adapter is None:
        raise ManifestError(f"{path}：未知适配器 {adapter_id}")
    media = contract.get("media")
    if not isinstance(media, list) or any(item not in ALLOWED_MEDIA for item in media):
        raise ManifestError(f"{path}：media 无效")
    if any(item not in adapter.get("supported_media", []) for item in media):
        raise ManifestError(f"{path}：适配器 {adapter_id} 不支持所选媒体")
    mappings = contract.get("mapping")
    if not isinstance(mappings, list) or not mappings:
        raise ManifestError(f"{path}：mapping 必须是非空数组")

    owner = contract.get("owner_logic")
    logic = logics.get(owner) if isinstance(owner, str) else None
    if owner is not None and logic is None:
        raise ManifestError(f"{path}：未知 owner_logic {owner}")
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
        raise ManifestError(f"{path}：event_types 必须是字符串数组")
    if logic is not None:
        if not event_types:
            raise ManifestError(f"{path}：Logic 所属模板必须声明 event_types")
        unknown_events = set(event_types) - declared_events
        if unknown_events:
            raise ManifestError(f"{path}：未声明的事件类型：{', '.join(sorted(unknown_events))}")

    transforms = set(adapter.get("transforms", []))
    targets = set()
    adapter_targets = set()
    for index, mapping in enumerate(mappings):
        if not isinstance(mapping, dict):
            raise ManifestError(f"{path}：mapping[{index}] 必须是对象")
        source = mapping.get("source")
        target = mapping.get("target")
        location = mapping.get("location", "body")
        transform = mapping.get("transform", "")
        if not isinstance(source, str) or not source or not isinstance(target, str) or not target:
            raise ManifestError(f"{path}：mapping[{index}] 必须包含 source 和 target")
        if location not in ALLOWED_LOCATIONS:
            raise ManifestError(f"{path}：mapping[{index}] 的 location 无效：{location}")
        if location != "body" and "." in target:
            raise ManifestError(f"{path}：非 body target 不能嵌套：{target}")
        if transform not in transforms:
            raise ManifestError(f"{path}：mapping[{index}] 使用了不支持的 transform：{transform}")
        if transform in ("base64", "data_url", "file") and not source.startswith("media."):
            raise ManifestError(f"{path}：文件内容 transform 需要 media 来源")
        if location == "file" and transform != "file":
            raise ManifestError(f"{path}：file location 要求 transform=file")
        target_key = f"{location}:{target}"
        if target_key in targets:
            raise ManifestError(f"{path}：target 重复：{target_key}")
        targets.add(target_key)
        if adapter_id == "dify_workflow" and target in adapter_targets:
            raise ManifestError(f"{path}：Dify 输入重复：{target}")
        adapter_targets.add(target)
        if adapter_id == "dify_workflow" and location not in ("body", "file"):
            raise ManifestError(f"{path}：Dify mapping 仅支持 body/file")
        if source == "constant":
            if "value" not in mapping:
                raise ManifestError(f"{path}：constant mapping[{index}] 需要 value")
        else:
            root = source.partition(".")[0]
            if root not in ALLOWED_SOURCE_ROOTS:
                raise ManifestError(f"{path}：mapping[{index}] 的 source 无效：{source}")
        if source.startswith("fields.") and logic is not None:
            field = source.split(".", 1)[1]
            if field not in declared_fields:
                raise ManifestError(f"{path}：所属 Logic 未声明字段 {field}")
        if source.startswith("media.") and source.split(".", 1)[1] not in media:
            raise ManifestError(f"{path}：mapping 来源 {source} 未在 media 中启用")
        if mapping.get("type", "") not in ALLOWED_TYPES:
            raise ManifestError(f"{path}：mapping[{index}] 的 type 无效")
        if transform == "file" and mapping.get("file_mode", "list") not in ("single", "list"):
            raise ManifestError(f"{path}：mapping[{index}] 的 file_mode 无效")

    if adapter_id == "http":
        request = contract.get("request")
        if not isinstance(request, dict):
            raise ManifestError(f"{path}：HTTP 模板需要 request")
        if request.get("method") not in ("POST", "PUT", "PATCH"):
            raise ManifestError(f"{path}：HTTP method 无效")
        if not isinstance(request.get("path"), str) or not request["path"]:
            raise ManifestError(f"{path}：HTTP request.path 为必填项")
        if request.get("encoding") not in ("json", "form", "multipart"):
            raise ManifestError(f"{path}：HTTP encoding 无效")
        locations = {mapping.get("location", "body") for mapping in mappings}
        if request["encoding"] == "json" and locations & {"form", "file"}:
            raise ManifestError(f"{path}：JSON request 不能包含 form/file mapping")
        if request["encoding"] == "form" and locations & {"body", "file"}:
            raise ManifestError(f"{path}：form request 仅接受 form/query/header mapping")
        if request["encoding"] == "multipart" and "body" in locations:
            json_part = request.get("json_part")
            if not isinstance(json_part, str) or not json_part.strip():
                raise ManifestError(f"{path}：multipart body mapping 需要 request.json_part")
            occupied = {
                str(mapping.get("target", "")) for mapping in mappings
                if mapping.get("location", "body") in ("form", "file")
            }
            if json_part in occupied:
                raise ManifestError(f"{path}：request.json_part 与 form/file target 冲突")
        if request["encoding"] == "multipart" and not locations & {"body", "file"}:
            raise ManifestError(f"{path}：multipart 需要文件或 JSON body 部分")


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
                raise ManifestError(f"{module_dir}/logic.json：report_templates 条目必须是字符串")
            path = (module_dir / relative).resolve()
            if module_dir.resolve() not in path.parents or not path.is_file():
                raise ManifestError(f"{module_dir}/logic.json：找不到上报模板 {relative}")
            declared_module_paths.add(path)
            contract = load_json(path)
            contract["id"] = resolved_report_template_id(path, contract, str(logic["name"]))
            sources.append((path, contract))
    all_module_paths = set(args.logic_root.glob("modules/*/report_templates/*.json")) | set(
        args.logic_root.glob("global_modules/*/report_templates/*.json")
    )
    undeclared = sorted(path for path in all_module_paths if path.resolve() not in declared_module_paths)
    if undeclared:
        raise ManifestError(f"存在未声明的模块上报模板：{undeclared[0]}")

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
            raise ManifestError(f"合约 ID 重复 {contract_id}：{seen[contract_id]} 与 {path}")
        seen[contract_id] = path

    args.output.mkdir(parents=True, exist_ok=True)
    for old in args.output.glob("*.json"):
        old.unlink()
    for path, contract in sources:
        atomic_write(args.output / f"{contract['id']}.json", contract)
    print(f"已收集 {len(sources)} 个上报模板到 {args.output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ManifestError, OSError, ValueError) as exc:
        print(f"[错误] {exc}", file=sys.stderr)
        raise SystemExit(1)
