#!/usr/bin/env python3
"""Validate module manifests and generate Web/C++ logic capability catalogs."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


REGISTER_LOGIC_RE = re.compile(
    r"\bREGISTER_LOGIC\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)"
)
REGISTER_LOGIC_ACTION_RE = re.compile(
    r"\bREGISTER_LOGIC_ACTION\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,"
)
REGISTER_GLOBAL_LOGIC_RE = re.compile(
    r"\bREGISTER_GLOBAL_LOGIC\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)"
)
REGISTER_GLOBAL_LOGIC_ACTION_RE = re.compile(
    r"\bREGISTER_GLOBAL_LOGIC_ACTION\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,"
)
PARAM_ACCESS_RE = re.compile(
    r'\bparam_(float|int|bool|string|json)\s*\(\s*"([^"]+)"'
)
OUTPUT_PUBLISH_RE = re.compile(
    r'\bpublish_(string|number|int|bool|json)\s*\(\s*"([^"]+)"'
)
REPORT_EVENT_CALL_RE = re.compile(r"\breport_event\s*\(")
EVENT_TYPE_LITERAL_RE = re.compile(r'\.event_type\s*=\s*"([^"]+)"')
EVENT_FIELD_LITERAL_RE = re.compile(
    r'\bevent_(?:json_)?field\s*\(\s*"([^"]+)"'
)
CPP_CODE_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp"}
CPP_SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx"}


class ManifestError(ValueError):
    pass


class DuplicateJsonKeyError(ValueError):
    pass


SCHEMA_TYPES = {"string", "number", "integer", "boolean", "array", "object"}
LOGIC_OUTPUT_TYPES = {"string", "number", "integer", "boolean", "json"}
RELOAD_POLICIES = {"preserve_state", "reset_state", "restart_required"}
JSON_SAFE_INTEGER_MAX = (1 << 53) - 1
REPORT_TEMPLATE_ID_RE = re.compile(r"^[A-Za-z0-9._-]+$")


def is_finite_json_number(value: Any) -> bool:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return False
    try:
        return math.isfinite(float(value))
    except (OverflowError, ValueError):
        return False


def json_value_matches_type(value: Any, schema_type: str) -> bool:
    if schema_type == "string":
        return isinstance(value, str)
    if schema_type == "number":
        return is_finite_json_number(value)
    if schema_type == "integer":
        return (
            isinstance(value, int)
            and not isinstance(value, bool)
            and abs(value) <= JSON_SAFE_INTEGER_MAX
        )
    if schema_type == "boolean":
        return isinstance(value, bool)
    if schema_type == "array":
        return isinstance(value, list)
    if schema_type == "object":
        return isinstance(value, dict)
    return False


def validate_parameter_schema(manifest_path: Path, schema: Any) -> Dict[str, Any]:
    if not isinstance(schema, dict):
        raise ManifestError(f"{manifest_path}：parameters 必须是 JSON Schema 对象")
    if schema.get("type") != "object":
        raise ManifestError(f"{manifest_path}：parameters.type 必须是 'object'")
    if schema.get("additionalProperties") is not False:
        raise ManifestError(
            f"{manifest_path}：parameters.additionalProperties 必须是 false"
        )
    properties = schema.get("properties", {})
    if not isinstance(properties, dict):
        raise ManifestError(f"{manifest_path}：parameters.properties 必须是对象")

    for key, spec in properties.items():
        prefix = f"{manifest_path}: parameters.properties.{key}"
        if not isinstance(key, str) or not key:
            raise ManifestError(f"{manifest_path}：参数键必须是非空字符串")
        if not isinstance(spec, dict):
            raise ManifestError(f"{prefix} 必须是对象")
        schema_type = spec.get("type")
        if schema_type not in SCHEMA_TYPES:
            raise ManifestError(
                f"{prefix}.type 必须是以下值之一：{', '.join(sorted(SCHEMA_TYPES))}"
            )
        if "default" not in spec:
            raise ManifestError(f"{prefix}.default 为必填项")
        if not json_value_matches_type(spec["default"], schema_type):
            raise ManifestError(f"{prefix}.default 与类型 '{schema_type}' 不匹配")

        policy = spec.get("x-hot-reload", "preserve_state")
        if policy not in RELOAD_POLICIES:
            raise ManifestError(
                f"{prefix}.x-hot-reload 必须是以下值之一：{', '.join(sorted(RELOAD_POLICIES))}"
            )

        for field in ("title", "description", "x-placeholder", "x-unit"):
            if field in spec and not isinstance(spec[field], str):
                raise ManifestError(f"{prefix}.{field} 必须是字符串")
        if "x-ui-hidden" in spec and not isinstance(spec["x-ui-hidden"], bool):
            raise ManifestError(f"{prefix}.x-ui-hidden 必须是布尔值")
        if "x-step" in spec:
            if schema_type not in {"number", "integer"}:
                raise ManifestError(f"{prefix}.x-step 要求参数为数值类型")
            if not json_value_matches_type(spec["x-step"], "number") or spec["x-step"] <= 0:
                raise ManifestError(f"{prefix}.x-step 必须是有限正数")
        if "x-widget" in spec and not (
            schema_type == "string" and spec["x-widget"] == "textarea"
        ):
            raise ManifestError(
                f"{prefix}.x-widget 当前仅支持字符串类型的 'textarea'"
            )

        enum = spec.get("enum")
        if enum is not None:
            if schema_type != "string":
                raise ManifestError(f"{prefix}.enum 当前仅支持字符串参数")
            if not isinstance(enum, list) or not enum:
                raise ManifestError(f"{prefix}.enum 必须是非空数组")
            if any(not json_value_matches_type(value, schema_type) for value in enum):
                raise ManifestError(f"{prefix}.enum 包含类型错误的值")
            if spec["default"] not in enum:
                raise ManifestError(f"{prefix}.default 必须存在于 enum 中")

        minimum = spec.get("minimum")
        maximum = spec.get("maximum")
        if (minimum is not None or maximum is not None) and schema_type not in {
            "number",
            "integer",
        }:
            raise ManifestError(f"{prefix}：minimum/maximum 要求参数为数值类型")
        if minimum is not None and not json_value_matches_type(minimum, "number"):
            raise ManifestError(f"{prefix}.minimum 必须是数字")
        if maximum is not None and not json_value_matches_type(maximum, "number"):
            raise ManifestError(f"{prefix}.maximum 必须是数字")
        if minimum is not None and maximum is not None and minimum > maximum:
            raise ManifestError(f"{prefix}.minimum 不能大于 maximum")
        if minimum is not None and spec["default"] < minimum:
            raise ManifestError(f"{prefix}.default 小于 minimum")
        if maximum is not None and spec["default"] > maximum:
            raise ManifestError(f"{prefix}.default 大于 maximum")

    return schema


def schema_property_to_web_param(key: str, spec: Dict[str, Any]) -> Dict[str, Any]:
    schema_type = spec["type"]
    if "enum" in spec:
        web_type = "enum"
    elif schema_type == "number":
        web_type = "float"
    elif schema_type == "integer":
        web_type = "int"
    elif schema_type == "boolean":
        web_type = "bool"
    elif schema_type in {"array", "object"}:
        web_type = "json"
    elif schema_type == "string" and spec.get("x-widget") == "textarea":
        web_type = "text"
    else:
        web_type = "string"

    out: Dict[str, Any] = {
        "key": key,
        "type": web_type,
        "default": spec["default"],
        "hot_reload": spec.get("x-hot-reload", "preserve_state"),
    }
    if schema_type in {"array", "object"}:
        out["json_type"] = schema_type
    mappings = {
        "title": "label",
        "description": "help",
        "minimum": "min",
        "maximum": "max",
        "enum": "options",
        "x-placeholder": "placeholder",
        "x-step": "step",
        "x-unit": "unit",
    }
    for source, target in mappings.items():
        if source in spec:
            out[target] = spec[source]
    return out


def web_params_from_schema(parameter_schema: Dict[str, Any]) -> List[Dict[str, Any]]:
    result: List[Dict[str, Any]] = []
    for key, spec in parameter_schema.get("properties", {}).items():
        if spec.get("x-ui-hidden") is True:
            continue
        result.append(schema_property_to_web_param(key, spec))
    return result


def load_object(path: Path) -> Dict[str, Any]:
    def reject_duplicate_keys(pairs: List[Tuple[str, Any]]) -> Dict[str, Any]:
        value: Dict[str, Any] = {}
        for key, item in pairs:
            if key in value:
                raise DuplicateJsonKeyError(key)
            value[key] = item
        return value

    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except DuplicateJsonKeyError as exc:
        raise ManifestError(f"{path}：JSON 键重复：{exc}") from exc
    except (OSError, json.JSONDecodeError) as exc:
        if isinstance(exc, json.JSONDecodeError):
            raise ManifestError(
                f"{path}：JSON 格式错误（第 {exc.lineno} 行，第 {exc.colno} 列）"
            ) from exc
        raise ManifestError(f"{path}：无法读取 JSON 文件") from exc
    if not isinstance(value, dict):
        raise ManifestError(f"{path}：顶层必须是 JSON 对象")
    return value


def require_unique_strings(
    manifest_path: Path,
    items: Any,
    collection_name: str,
    key: str,
) -> None:
    if items is None:
        return
    if not isinstance(items, list):
        raise ManifestError(f"{manifest_path}：{collection_name} 必须是数组")

    seen = set()
    for index, item in enumerate(items):
        if not isinstance(item, dict):
            raise ManifestError(
                f"{manifest_path}：{collection_name}[{index}] 必须是对象"
            )
        value = item.get(key)
        if not isinstance(value, str) or not value:
            raise ManifestError(
                f"{manifest_path}：{collection_name}[{index}].{key} 必须是非空字符串"
            )
        if value in seen:
            raise ManifestError(
                f"{manifest_path}：{collection_name} 中的 {key} 重复：{value}"
            )
        seen.add(value)


def validate_event_types(
    manifest_path: Path, module_dir: Path, event_types: Any
) -> None:
    if event_types is None:
        raise ManifestError(
            f"{manifest_path}：event_types 为必填项；Logic 不上报事件时请使用 []"
        )
    require_unique_strings(manifest_path, event_types, "event_types", "id")
    declared = {item["id"] for item in event_types}
    for index, item in enumerate(event_types):
        for field in ("label", "help"):
            if field in item and not isinstance(item[field], str):
                raise ManifestError(
                    f"{manifest_path}：event_types[{index}].{field} 必须是字符串"
                )

    source_text = ""
    for source in module_cpp_code_files(module_dir):
        try:
            source_text += source.read_text(encoding="utf-8") + "\n"
        except OSError as exc:
            raise ManifestError(f"{source}：无法读取源文件") from exc
    if REPORT_EVENT_CALL_RE.search(source_text) and not event_types:
        raise ManifestError(
            f"{manifest_path}：Logic 调用了 report_event()，但 event_types 为空"
        )
    undeclared_literals = sorted(set(EVENT_TYPE_LITERAL_RE.findall(source_text)) - declared)
    if undeclared_literals:
        raise ManifestError(
            f"{manifest_path}：C++ 使用了未声明的事件类型："
            f"{', '.join(undeclared_literals)}"
        )


def validate_logic_outputs(manifest_path: Path, outputs: Any) -> None:
    if outputs is None:
        return
    require_unique_strings(manifest_path, outputs, "outputs", "key")
    for index, item in enumerate(outputs):
        output_type = item.get("type")
        if output_type not in LOGIC_OUTPUT_TYPES:
            raise ManifestError(
                f"{manifest_path}：outputs[{index}].type 必须是以下值之一："
                f"{', '.join(sorted(LOGIC_OUTPUT_TYPES))}"
            )
        for field in ("label", "help"):
            if field in item and not isinstance(item[field], str):
                raise ManifestError(
                    f"{manifest_path}：outputs[{index}].{field} 必须是字符串"
                )


def validate_output_publications(
    manifest_path: Path, module_dir: Path, outputs: Any
) -> None:
    declarations = {
        item["key"]: item["type"] for item in (outputs or []) if isinstance(item, dict)
    }
    expected_types = {
        "string": "string",
        "number": "number",
        "int": "integer",
        "bool": "boolean",
        "json": "json",
    }
    for source in module_cpp_code_files(module_dir):
        try:
            text = source.read_text(encoding="utf-8")
        except OSError as exc:
            raise ManifestError(f"{source}：无法读取源文件") from exc
        for accessor, key in OUTPUT_PUBLISH_RE.findall(text):
            declared = declarations.get(key)
            if declared is None:
                raise ManifestError(
                    f'{source}：publish_{accessor}("{key}") 没有匹配的 outputs 条目'
                )
            if declared != expected_types[accessor]:
                raise ManifestError(
                    f'{source}：publish_{accessor}("{key}") 与输出类型不匹配：'
                    f"'{declared}'"
                )


def module_cpp_source_files(module_dir: Path) -> List[Path]:
    return sorted(
        path
        for path in module_dir.rglob("*")
        if path.is_file() and path.suffix.lower() in CPP_SOURCE_SUFFIXES
    )


def registered_logic_names(module_dir: Path) -> List[str]:
    names: List[str] = []
    for source in module_cpp_source_files(module_dir):
        try:
            names.extend(REGISTER_LOGIC_RE.findall(source.read_text(encoding="utf-8")))
        except OSError as exc:
            raise ManifestError(f"{source}：无法读取源文件") from exc
    return names


def registered_action_logic_names(module_dir: Path) -> List[str]:
    names: List[str] = []
    for source in module_cpp_source_files(module_dir):
        try:
            names.extend(
                REGISTER_LOGIC_ACTION_RE.findall(source.read_text(encoding="utf-8"))
            )
        except OSError as exc:
            raise ManifestError(f"{source}：无法读取源文件") from exc
    return names


def registered_global_logic_names(module_dir: Path) -> List[str]:
    names: List[str] = []
    for source in module_cpp_source_files(module_dir):
        try:
            names.extend(
                REGISTER_GLOBAL_LOGIC_RE.findall(source.read_text(encoding="utf-8"))
            )
        except OSError as exc:
            raise ManifestError(f"{source}：无法读取源文件") from exc
    return names


def registered_global_action_logic_names(module_dir: Path) -> List[str]:
    names: List[str] = []
    for source in module_cpp_source_files(module_dir):
        try:
            names.extend(
                REGISTER_GLOBAL_LOGIC_ACTION_RE.findall(
                    source.read_text(encoding="utf-8")
                )
            )
        except OSError as exc:
            raise ManifestError(f"{source}：无法读取源文件") from exc
    return names


def module_cpp_code_files(module_dir: Path) -> List[Path]:
    return sorted(
        path
        for path in module_dir.rglob("*")
        if path.is_file() and path.suffix.lower() in CPP_CODE_SUFFIXES
    )


def validate_parameter_accesses(
    manifest_path: Path, module_dir: Path, parameter_schema: Dict[str, Any]
) -> None:
    expected_types = {
        "float": {"number", "integer"},
        "int": {"integer"},
        "bool": {"boolean"},
        "string": {"string"},
        "json": {"array", "object"},
    }
    properties = parameter_schema.get("properties", {})
    for source in module_cpp_code_files(module_dir):
        try:
            text = source.read_text(encoding="utf-8")
        except OSError as exc:
            raise ManifestError(f"{source}：无法读取源文件") from exc
        for accessor, key in PARAM_ACCESS_RE.findall(text):
            spec = properties.get(key)
            if not isinstance(spec, dict):
                raise ManifestError(
                    f"{source}：param_{accessor}(\"{key}\") 没有匹配的 parameters.properties 条目"
                )
            if spec.get("type") not in expected_types[accessor]:
                raise ManifestError(
                    f"{source}：param_{accessor}(\"{key}\") 与 Schema 类型 '{spec.get('type')}' 不匹配"
                )


def validate_report_fields(
    manifest_path: Path, module_dir: Path, report_fields: Any
) -> None:
    require_unique_strings(manifest_path, report_fields, "report_fields", "key")
    declared = {
        item["key"] for item in (report_fields or []) if isinstance(item, dict)
    }
    for index, item in enumerate(report_fields or []):
        if item.get("type") not in {"string", "number", "boolean", "json"}:
            raise ManifestError(
                f"{manifest_path}：report_fields[{index}].type 必须是以下值之一："
                "boolean, json, number, string"
            )
        for field in ("label", "help"):
            if field in item and not isinstance(item[field], str):
                raise ManifestError(
                    f"{manifest_path}：report_fields[{index}].{field} 必须是字符串"
                )

    used = set()
    for source in module_cpp_code_files(module_dir):
        try:
            used.update(EVENT_FIELD_LITERAL_RE.findall(source.read_text(encoding="utf-8")))
        except OSError as exc:
            raise ManifestError(f"{source}：无法读取源文件") from exc
    undeclared = sorted(used - declared)
    unassigned = sorted(declared - used)
    if undeclared:
        raise ManifestError(
            f"{manifest_path}：C++ 赋值了未声明的上报字段："
            f"{', '.join(undeclared)}"
        )
    if unassigned:
        raise ManifestError(
            f"{manifest_path}：以下 report_fields 未由 C++ event_field() 赋值："
            f"{', '.join(unassigned)}"
        )


def declared_report_template_ids(
    manifest_path: Path,
    module_dir: Path,
    manifest: Dict[str, Any],
    logic_name: str,
) -> List[str]:
    """Resolve only the templates explicitly declared by this Logic module."""
    entries = manifest.get("report_templates", [])
    if not isinstance(entries, list):
        raise ManifestError(f"{manifest_path}：report_templates 必须是数组")
    result: List[str] = []
    seen = set()
    module_root = module_dir.resolve()
    for index, relative in enumerate(entries):
        if not isinstance(relative, str) or not relative.strip():
            raise ManifestError(
                f"{manifest_path}：report_templates[{index}] 必须是非空字符串"
            )
        template_path = (module_dir / relative).resolve()
        if module_root not in template_path.parents or not template_path.is_file():
            raise ManifestError(
                f"{manifest_path}：找不到上报模板 {relative}"
            )
        template = load_object(template_path)
        template_id = resolved_report_template_id(template_path, template, logic_name)
        if template.get("owner_logic") != logic_name:
            raise ManifestError(
                f"{template_path}：owner_logic 必须是 {logic_name}"
            )
        if template_id in seen:
            raise ManifestError(
                f"{manifest_path}：上报模板 ID 重复：{template_id}"
            )
        seen.add(template_id)
        result.append(template_id)
    return result


def resolved_report_template_id(
    template_path: Path, template: Dict[str, Any], logic_name: str
) -> str:
    """Return the hidden runtime key; source templates normally omit it."""
    explicit = template.get("id")
    template_id = explicit.strip() if isinstance(explicit, str) else ""
    if not template_id:
        safe_stem = re.sub(r"[^A-Za-z0-9._-]+", "_", template_path.stem).strip("._-")
        if not safe_stem:
            digest = hashlib.sha256(template_path.name.encode("utf-8")).hexdigest()[:12]
            safe_stem = f"template_{digest}"
        template_id = f"{logic_name}.{safe_stem}"
    if not REPORT_TEMPLATE_ID_RE.fullmatch(template_id):
        raise ManifestError(f"{template_path}：生成的模板键无效")
    return template_id


def load_channel_manifests(logic_root: Path) -> List[Dict[str, Any]]:
    module_root = logic_root / "modules"
    module_dirs = sorted(path for path in module_root.iterdir() if path.is_dir())

    result: List[Dict[str, Any]] = []
    seen_names = set()
    for module_dir in module_dirs:
        manifest_path = module_dir / "logic.json"
        if not manifest_path.is_file():
            raise ManifestError(f"{module_dir}：通道 Logic 模块缺少 logic.json")

        manifest = load_object(manifest_path)
        if "name" in manifest:
            raise ManifestError(
                f"{manifest_path}：name 由 REGISTER_LOGIC(func) 自动生成，请删除该字段"
            )

        registrations = registered_logic_names(module_dir)
        if len(registrations) != 1:
            found = ", ".join(sorted(registrations)) or "无"
            raise ManifestError(
                f"{manifest_path}：模块必须且只能包含一个 REGISTER_LOGIC(func) "
                f"（实际找到：{found}）"
            )
        name = registrations[0]
        if name in seen_names:
            raise ManifestError(f"{manifest_path}：通道 Logic 函数重复：{name}")
        seen_names.add(name)

        action_registrations = registered_action_logic_names(module_dir)
        if len(action_registrations) > 1 or any(
            registered_name != name for registered_name in action_registrations
        ):
            found = ", ".join(sorted(action_registrations))
            raise ManifestError(
                f"{manifest_path}：REGISTER_LOGIC_ACTION 必须引用函数 "
                f"'{name}'（实际找到：{found}）"
            )
        require_unique_strings(manifest_path, manifest.get("actions"), "actions", "id")
        if bool(manifest.get("actions")) != bool(action_registrations):
            raise ManifestError(
                f"{manifest_path}：actions 与 REGISTER_LOGIC_ACTION 必须同时存在或同时不存在"
            )

        parameter_schema = validate_parameter_schema(
            manifest_path, manifest.get("parameters")
        )
        if "params" in manifest:
            raise ManifestError(
                f"{manifest_path}：不支持顶层 params，请仅声明 parameters.properties"
            )
        validate_parameter_accesses(manifest_path, module_dir, parameter_schema)
        manifest["report_template_ids"] = declared_report_template_ids(
            manifest_path, module_dir, manifest, name,
        )
        manifest["params"] = web_params_from_schema(parameter_schema)
        validate_event_types(manifest_path, module_dir, manifest.get("event_types"))
        validate_logic_outputs(manifest_path, manifest.get("outputs"))
        validate_output_publications(manifest_path, module_dir, manifest.get("outputs"))
        validate_report_fields(manifest_path, module_dir, manifest.get("report_fields"))
        require_unique_strings(
            manifest_path, manifest.get("business_fields"), "business_fields", "path"
        )
        result.append({"name": name, **manifest})

    return result


def load_global_manifests(logic_root: Path) -> List[Dict[str, Any]]:
    module_root = logic_root / "global_modules"
    if not module_root.is_dir():
        raise ManifestError(f"{module_root}：找不到全局 Logic 模块根目录")
    module_dirs = sorted(path for path in module_root.iterdir() if path.is_dir())

    result: List[Dict[str, Any]] = []
    seen_names = set()
    for module_dir in module_dirs:
        manifest_path = module_dir / "logic.json"
        if not manifest_path.is_file():
            raise ManifestError(f"{module_dir}：全局 Logic 模块缺少 logic.json")

        manifest = load_object(manifest_path)
        if "name" in manifest:
            raise ManifestError(
                f"{manifest_path}：name 由 REGISTER_GLOBAL_LOGIC(func) 自动生成，请删除该字段"
            )

        registrations = registered_global_logic_names(module_dir)
        if len(registrations) != 1:
            found = ", ".join(sorted(registrations)) or "无"
            raise ManifestError(
                f"{manifest_path}：模块必须且只能包含一个 "
                f"REGISTER_GLOBAL_LOGIC(func)（实际找到：{found}）"
            )
        name = registrations[0]
        if name in seen_names:
            raise ManifestError(f"{manifest_path}：全局 Logic 函数重复：{name}")
        seen_names.add(name)

        action_registrations = registered_global_action_logic_names(module_dir)
        if len(action_registrations) > 1 or any(
            registered_name != name for registered_name in action_registrations
        ):
            found = ", ".join(sorted(action_registrations))
            raise ManifestError(
                f"{manifest_path}：REGISTER_GLOBAL_LOGIC_ACTION 必须引用函数 "
                f"'{name}'（实际找到：{found}）"
            )
        require_unique_strings(manifest_path, manifest.get("actions"), "actions", "id")
        if bool(manifest.get("actions")) != bool(action_registrations):
            raise ManifestError(
                f"{manifest_path}：actions 与 REGISTER_GLOBAL_LOGIC_ACTION 必须同时存在或同时不存在"
            )

        parameter_schema = validate_parameter_schema(
            manifest_path, manifest.get("parameters")
        )
        if "params" in manifest:
            raise ManifestError(
                f"{manifest_path}：不支持顶层 params，请仅声明 parameters.properties"
            )
        validate_parameter_accesses(manifest_path, module_dir, parameter_schema)
        manifest["report_template_ids"] = declared_report_template_ids(
            manifest_path, module_dir, manifest, name,
        )
        manifest["params"] = web_params_from_schema(parameter_schema)
        validate_event_types(manifest_path, module_dir, manifest.get("event_types"))
        validate_report_fields(manifest_path, module_dir, manifest.get("report_fields"))
        require_unique_strings(
            manifest_path, manifest.get("business_fields"), "business_fields", "path"
        )
        result.append({"name": name, **manifest})

    return result


def validate_named_list(catalog_path: Path, value: Any, key: str) -> List[Any]:
    if not isinstance(value, list):
        raise ManifestError(f"{catalog_path}：{key} 必须是数组")
    seen = set()
    for index, item in enumerate(value):
        if isinstance(item, str):
            name = item
        elif isinstance(item, dict):
            name = item.get("name")
        else:
            name = None
        if not isinstance(name, str) or not name:
            raise ManifestError(f"{catalog_path}：{key}[{index}] 没有有效名称")
        if name in seen:
            raise ManifestError(f"{catalog_path}：{key} 名称重复：{name}")
        seen.add(name)
    return value


def build_catalog(logic_root: Path, engine_catalog: Optional[Path] = None) -> Dict[str, Any]:
    catalog_path = engine_catalog or Path(__file__).resolve().parents[1] / "metadata" / "catalog.json"
    shared = load_object(catalog_path)
    if "channel_logics" in shared:
        raise ManifestError(
            f"{catalog_path}：channel_logics 应定义在 modules/*/logic.json 中"
        )
    if "global_logics" in shared:
        raise ManifestError(
            f"{catalog_path}：global_logics 应定义在 global_modules/*/logic.json 中"
        )

    modules = load_channel_manifests(logic_root)
    global_logics = load_global_manifests(logic_root)
    channel_names = {item["name"] for item in modules}
    global_names = {item["name"] for item in global_logics}
    collisions = sorted(channel_names & global_names)
    if collisions:
        raise ManifestError(
            f"{logic_root}：通道/全局 Logic ID 必须唯一：{', '.join(collisions)}"
        )
    model_types = validate_named_list(
        catalog_path, shared.get("model_types", []), "model_types"
    )

    return {
        "_comment": (
            "Generated from channel/global registration macros, module logic.json "
            "files and engine metadata/catalog.json; do not edit the generated file."
        ),
        "channel_logics": modules,
        "global_logics": global_logics,
        "model_types": model_types,
    }


def atomic_write_json(path: Path, value: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            json.dump(value, handle, ensure_ascii=False, indent=4, allow_nan=False)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp_name, path)
    except BaseException:
        try:
            os.unlink(tmp_name)
        except FileNotFoundError:
            pass
        raise


def atomic_write_text(path: Path, text: str) -> None:
    try:
        if path.read_text(encoding="utf-8") == text:
            # OUTPUT 必须比依赖新，否则 Make 在同一轮并行构建中可能重复执行
            # 生成命令，后续增量构建也会每次重新校验。
            os.utime(path, None)
            return
    except OSError:
        pass
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(text)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp_name, path)
    except BaseException:
        try:
            os.unlink(tmp_name)
        except FileNotFoundError:
            pass
        raise


def render_cpp_catalog(catalog: Dict[str, Any]) -> str:
    # C++ 只需要运行时校验信息，不把 actions/report_fields 等 Web 元数据
    # 重复编进二进制。完整能力清单仍由 --output 生成给 Web 使用。
    runtime_catalog = {
        "channel_logics": [
            {"name": item["name"], "parameters": item["parameters"]}
            for item in catalog["channel_logics"]
        ],
        "global_logics": [
            {"name": item["name"], "parameters": item["parameters"]}
            for item in catalog["global_logics"]
        ],
    }
    payload = json.dumps(
        runtime_catalog,
        ensure_ascii=False,
        separators=(",", ":"),
        allow_nan=False,
    )
    delimiter = "RK_LOGIC_CATALOG"
    while f"){delimiter}\"" in payload:
        delimiter += "_X"
    return (
        "// Generated by scripts/generate_logics_catalog.py; do not edit.\n"
        "const char *logic_embedded_catalog_json()\n"
        "{\n"
        f"    return R\"{delimiter}({payload}){delimiter}\";\n"
        "}\n"
    )


def parse_args(argv: Iterable[str]) -> argparse.Namespace:
    project_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--logic-root",
        type=Path,
        default=project_root.parent / "projects" / "person_count" / "logic",
        help="external project logic root (default: projects/person_count/logic)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="write generated catalog atomically; omit to print it",
    )
    parser.add_argument(
        "--cpp-output",
        type=Path,
        help="write a C++ source that embeds the validated catalog",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="validate manifests without writing or printing the catalog",
    )
    return parser.parse_args(argv)


def main(argv: Optional[Iterable[str]] = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])
    try:
        catalog = build_catalog(args.logic_root.resolve())
        if args.cpp_output:
            atomic_write_text(args.cpp_output.resolve(), render_cpp_catalog(catalog))
        if args.output:
            atomic_write_json(args.output.resolve(), catalog)
        if args.check:
            print(
                f"validated {len(catalog['channel_logics'])} channel and "
                f"{len(catalog['global_logics'])} global logic manifest(s)",
                file=sys.stderr,
            )
        elif not args.output and not args.cpp_output:
            json.dump(catalog, sys.stdout, ensure_ascii=False, indent=4)
            sys.stdout.write("\n")
    except ManifestError as exc:
        print(f"logic catalog error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
