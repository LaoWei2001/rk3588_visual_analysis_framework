#!/usr/bin/env python3
"""Validate module manifests and generate Web/C++ logic capability catalogs."""

from __future__ import annotations

import argparse
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
PARAM_ACCESS_RE = re.compile(
    r'\bparam_(float|int|bool|string|json)\s*\(\s*"([^"]+)"'
)
REPORT_EVENT_CALL_RE = re.compile(r"\breport_event\s*\(")
EVENT_TYPE_LITERAL_RE = re.compile(r'\.event_type\s*=\s*"([^"]+)"')
CPP_CODE_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp"}
CPP_SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx"}


class ManifestError(ValueError):
    pass


class DuplicateJsonKeyError(ValueError):
    pass


SCHEMA_TYPES = {"string", "number", "integer", "boolean", "array", "object"}
RELOAD_POLICIES = {"preserve_state", "reset_state", "restart_required"}
JSON_SAFE_INTEGER_MAX = (1 << 53) - 1


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
        raise ManifestError(f"{manifest_path}: parameters must be a JSON Schema object")
    if schema.get("type") != "object":
        raise ManifestError(f"{manifest_path}: parameters.type must be 'object'")
    if schema.get("additionalProperties") is not False:
        raise ManifestError(
            f"{manifest_path}: parameters.additionalProperties must be false"
        )
    properties = schema.get("properties", {})
    if not isinstance(properties, dict):
        raise ManifestError(f"{manifest_path}: parameters.properties must be an object")

    for key, spec in properties.items():
        prefix = f"{manifest_path}: parameters.properties.{key}"
        if not isinstance(key, str) or not key:
            raise ManifestError(f"{manifest_path}: parameter keys must be non-empty strings")
        if not isinstance(spec, dict):
            raise ManifestError(f"{prefix} must be an object")
        schema_type = spec.get("type")
        if schema_type not in SCHEMA_TYPES:
            raise ManifestError(
                f"{prefix}.type must be one of {', '.join(sorted(SCHEMA_TYPES))}"
            )
        if "default" not in spec:
            raise ManifestError(f"{prefix}.default is required")
        if not json_value_matches_type(spec["default"], schema_type):
            raise ManifestError(f"{prefix}.default does not match type '{schema_type}'")

        policy = spec.get("x-hot-reload", "preserve_state")
        if policy not in RELOAD_POLICIES:
            raise ManifestError(
                f"{prefix}.x-hot-reload must be one of {', '.join(sorted(RELOAD_POLICIES))}"
            )

        for field in ("title", "description", "x-placeholder", "x-unit"):
            if field in spec and not isinstance(spec[field], str):
                raise ManifestError(f"{prefix}.{field} must be a string")
        if "x-ui-hidden" in spec and not isinstance(spec["x-ui-hidden"], bool):
            raise ManifestError(f"{prefix}.x-ui-hidden must be a boolean")
        if "x-step" in spec:
            if schema_type not in {"number", "integer"}:
                raise ManifestError(f"{prefix}.x-step requires a numeric type")
            if not json_value_matches_type(spec["x-step"], "number") or spec["x-step"] <= 0:
                raise ManifestError(f"{prefix}.x-step must be a positive finite number")
        if "x-widget" in spec and not (
            schema_type == "string" and spec["x-widget"] == "textarea"
        ):
            raise ManifestError(
                f"{prefix}.x-widget currently only supports 'textarea' for strings"
            )

        enum = spec.get("enum")
        if enum is not None:
            if schema_type != "string":
                raise ManifestError(f"{prefix}.enum currently supports string parameters only")
            if not isinstance(enum, list) or not enum:
                raise ManifestError(f"{prefix}.enum must be a non-empty array")
            if any(not json_value_matches_type(value, schema_type) for value in enum):
                raise ManifestError(f"{prefix}.enum contains a value of the wrong type")
            if spec["default"] not in enum:
                raise ManifestError(f"{prefix}.default must be present in enum")

        minimum = spec.get("minimum")
        maximum = spec.get("maximum")
        if (minimum is not None or maximum is not None) and schema_type not in {
            "number",
            "integer",
        }:
            raise ManifestError(f"{prefix}: minimum/maximum require a numeric type")
        if minimum is not None and not json_value_matches_type(minimum, "number"):
            raise ManifestError(f"{prefix}.minimum must be a number")
        if maximum is not None and not json_value_matches_type(maximum, "number"):
            raise ManifestError(f"{prefix}.maximum must be a number")
        if minimum is not None and maximum is not None and minimum > maximum:
            raise ManifestError(f"{prefix}.minimum must not exceed maximum")
        if minimum is not None and spec["default"] < minimum:
            raise ManifestError(f"{prefix}.default is below minimum")
        if maximum is not None and spec["default"] > maximum:
            raise ManifestError(f"{prefix}.default is above maximum")

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
        raise ManifestError(f"{path}: duplicate JSON key: {exc}") from exc
    except (OSError, json.JSONDecodeError) as exc:
        raise ManifestError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise ManifestError(f"{path}: top level must be a JSON object")
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
        raise ManifestError(f"{manifest_path}: {collection_name} must be an array")

    seen = set()
    for index, item in enumerate(items):
        if not isinstance(item, dict):
            raise ManifestError(
                f"{manifest_path}: {collection_name}[{index}] must be an object"
            )
        value = item.get(key)
        if not isinstance(value, str) or not value:
            raise ManifestError(
                f"{manifest_path}: {collection_name}[{index}].{key} must be a non-empty string"
            )
        if value in seen:
            raise ManifestError(
                f"{manifest_path}: duplicate {collection_name} {key}: {value}"
            )
        seen.add(value)


def validate_event_types(
    manifest_path: Path, module_dir: Path, event_types: Any
) -> None:
    if event_types is None:
        raise ManifestError(
            f"{manifest_path}: event_types is required; use [] when the logic never reports events"
        )
    require_unique_strings(manifest_path, event_types, "event_types", "id")
    declared = {item["id"] for item in event_types}
    for index, item in enumerate(event_types):
        for field in ("label", "help"):
            if field in item and not isinstance(item[field], str):
                raise ManifestError(
                    f"{manifest_path}: event_types[{index}].{field} must be a string"
                )

    source_text = ""
    for source in module_cpp_code_files(module_dir):
        try:
            source_text += source.read_text(encoding="utf-8") + "\n"
        except OSError as exc:
            raise ManifestError(f"{source}: cannot read source: {exc}") from exc
    if REPORT_EVENT_CALL_RE.search(source_text) and not event_types:
        raise ManifestError(
            f"{manifest_path}: logic calls report_event() but event_types is empty"
        )
    undeclared_literals = sorted(set(EVENT_TYPE_LITERAL_RE.findall(source_text)) - declared)
    if undeclared_literals:
        raise ManifestError(
            f"{manifest_path}: C++ uses undeclared event type(s): "
            f"{', '.join(undeclared_literals)}"
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
            raise ManifestError(f"{source}: cannot read source: {exc}") from exc
    return names


def registered_action_logic_names(module_dir: Path) -> List[str]:
    names: List[str] = []
    for source in module_cpp_source_files(module_dir):
        try:
            names.extend(
                REGISTER_LOGIC_ACTION_RE.findall(source.read_text(encoding="utf-8"))
            )
        except OSError as exc:
            raise ManifestError(f"{source}: cannot read source: {exc}") from exc
    return names


def registered_global_logic_names(module_dir: Path) -> List[str]:
    names: List[str] = []
    for source in module_cpp_source_files(module_dir):
        try:
            names.extend(
                REGISTER_GLOBAL_LOGIC_RE.findall(source.read_text(encoding="utf-8"))
            )
        except OSError as exc:
            raise ManifestError(f"{source}: cannot read source: {exc}") from exc
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
            raise ManifestError(f"{source}: cannot read source: {exc}") from exc
        for accessor, key in PARAM_ACCESS_RE.findall(text):
            spec = properties.get(key)
            if not isinstance(spec, dict):
                raise ManifestError(
                    f"{source}: param_{accessor}(\"{key}\") has no matching parameters.properties entry"
                )
            if spec.get("type") not in expected_types[accessor]:
                raise ManifestError(
                    f"{source}: param_{accessor}(\"{key}\") does not match Schema type '{spec.get('type')}'"
                )


def load_channel_manifests(logic_root: Path) -> List[Dict[str, Any]]:
    module_root = logic_root / "modules"
    module_dirs = sorted(path for path in module_root.iterdir() if path.is_dir())

    result: List[Dict[str, Any]] = []
    seen_names = set()
    for module_dir in module_dirs:
        manifest_path = module_dir / "logic.json"
        if not manifest_path.is_file():
            raise ManifestError(f"{module_dir}: channel logic module is missing logic.json")

        manifest = load_object(manifest_path)
        if "name" in manifest:
            raise ManifestError(
                f"{manifest_path}: name is generated from REGISTER_LOGIC(func); remove it"
            )

        registrations = registered_logic_names(module_dir)
        if len(registrations) != 1:
            found = ", ".join(sorted(registrations)) or "none"
            raise ManifestError(
                f"{manifest_path}: module must contain exactly one REGISTER_LOGIC(func) "
                f"(found: {found})"
            )
        name = registrations[0]
        if name in seen_names:
            raise ManifestError(f"{manifest_path}: duplicate channel logic function: {name}")
        seen_names.add(name)

        action_registrations = registered_action_logic_names(module_dir)
        if len(action_registrations) > 1 or any(
            registered_name != name for registered_name in action_registrations
        ):
            found = ", ".join(sorted(action_registrations))
            raise ManifestError(
                f"{manifest_path}: REGISTER_LOGIC_ACTION must reference function "
                f"'{name}' (found: {found})"
            )

        parameter_schema = validate_parameter_schema(
            manifest_path, manifest.get("parameters")
        )
        if "params" in manifest:
            raise ManifestError(
                f"{manifest_path}: top-level params is not supported; "
                "declare parameters.properties only"
            )
        validate_parameter_accesses(manifest_path, module_dir, parameter_schema)
        manifest["params"] = web_params_from_schema(parameter_schema)
        validate_event_types(manifest_path, module_dir, manifest.get("event_types"))
        require_unique_strings(manifest_path, manifest.get("actions"), "actions", "id")
        require_unique_strings(
            manifest_path, manifest.get("report_fields"), "report_fields", "key"
        )
        require_unique_strings(
            manifest_path, manifest.get("business_fields"), "business_fields", "path"
        )
        result.append({"name": name, **manifest})

    return result


def load_global_manifests(logic_root: Path) -> List[Dict[str, Any]]:
    module_root = logic_root / "global_modules"
    if not module_root.is_dir():
        raise ManifestError(f"{module_root}: global logic module root is missing")
    module_dirs = sorted(path for path in module_root.iterdir() if path.is_dir())

    result: List[Dict[str, Any]] = []
    seen_names = set()
    for module_dir in module_dirs:
        manifest_path = module_dir / "logic.json"
        if not manifest_path.is_file():
            raise ManifestError(f"{module_dir}: global logic module is missing logic.json")

        manifest = load_object(manifest_path)
        if "name" in manifest:
            raise ManifestError(
                f"{manifest_path}: name is generated from REGISTER_GLOBAL_LOGIC(func); remove it"
            )

        registrations = registered_global_logic_names(module_dir)
        if len(registrations) != 1:
            found = ", ".join(sorted(registrations)) or "none"
            raise ManifestError(
                f"{manifest_path}: module must contain exactly one "
                f"REGISTER_GLOBAL_LOGIC(func) (found: {found})"
            )
        name = registrations[0]
        if name in seen_names:
            raise ManifestError(f"{manifest_path}: duplicate global logic function: {name}")
        seen_names.add(name)

        parameter_schema = validate_parameter_schema(
            manifest_path, manifest.get("parameters")
        )
        if "params" in manifest:
            raise ManifestError(
                f"{manifest_path}: top-level params is not supported; "
                "declare parameters.properties only"
            )
        validate_parameter_accesses(manifest_path, module_dir, parameter_schema)
        manifest["params"] = web_params_from_schema(parameter_schema)
        result.append({"name": name, **manifest})

    return result


def validate_named_list(catalog_path: Path, value: Any, key: str) -> List[Any]:
    if not isinstance(value, list):
        raise ManifestError(f"{catalog_path}: {key} must be an array")
    seen = set()
    for index, item in enumerate(value):
        if isinstance(item, str):
            name = item
        elif isinstance(item, dict):
            name = item.get("name")
        else:
            name = None
        if not isinstance(name, str) or not name:
            raise ManifestError(f"{catalog_path}: {key}[{index}] has no valid name")
        if name in seen:
            raise ManifestError(f"{catalog_path}: duplicate {key} name: {name}")
        seen.add(name)
    return value


def build_catalog(logic_root: Path) -> Dict[str, Any]:
    catalog_path = logic_root / "catalog.json"
    shared = load_object(catalog_path)
    if "channel_logics" in shared:
        raise ManifestError(
            f"{catalog_path}: channel_logics belongs in modules/*/logic.json"
        )
    if "global_logics" in shared:
        raise ManifestError(
            f"{catalog_path}: global_logics belongs in global_modules/*/logic.json"
        )

    modules = load_channel_manifests(logic_root)
    global_logics = load_global_manifests(logic_root)
    channel_names = {item["name"] for item in modules}
    global_names = {item["name"] for item in global_logics}
    collisions = sorted(channel_names & global_names)
    if collisions:
        raise ManifestError(
            f"{logic_root}: channel/global logic IDs must be unique: {', '.join(collisions)}"
        )
    model_types = validate_named_list(
        catalog_path, shared.get("model_types", []), "model_types"
    )

    return {
        "_comment": (
            "Generated from channel/global registration macros, module logic.json "
            "files and src/logic/catalog.json; do not edit the generated file."
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
        default=project_root / "src" / "logic",
        help="logic source root (default: project src/logic)",
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
