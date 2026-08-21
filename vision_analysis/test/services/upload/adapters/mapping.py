import base64
import json
import mimetypes
import os
from typing import Any, Dict, Iterable


MISSING = object()
MAPPING_LOCATIONS = {"body", "query", "form", "header", "file"}


def lookup(event: Dict[str, Any], source: str) -> Any:
    if source == "constant":
        return MISSING
    current: Any = event
    for part in (part for part in source.split(".") if part):
        if not isinstance(current, dict) or part not in current:
            return MISSING
        current = current[part]
    return current


def set_path(root: Dict[str, Any], path: str, value: Any) -> None:
    parts = [part for part in path.split(".") if part]
    if not parts:
        raise ValueError("mapping target is empty")
    current = root
    for part in parts[:-1]:
        child = current.get(part)
        if not isinstance(child, dict):
            child = {}
            current[part] = child
        current = child
    current[parts[-1]] = value


def coerce(value: Any, value_type: str) -> Any:
    if value is None or not value_type:
        return value
    if value_type == "string":
        return str(value)
    if value_type == "number":
        number = float(value)
        return int(number) if number.is_integer() else number
    if value_type == "boolean":
        if isinstance(value, bool):
            return value
        text = str(value).strip().lower()
        if text in ("true", "1", "yes", "on"):
            return True
        if text in ("false", "0", "no", "off"):
            return False
        raise ValueError(f"invalid boolean value: {value}")
    if value_type == "json":
        return json.loads(value) if isinstance(value, str) else value
    raise ValueError(f"unsupported mapping type: {value_type}")


def transform_value(value: Any, transform: str, preview: bool = False) -> Any:
    if not transform:
        return value
    if transform == "json_string":
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    if transform in ("base64", "data_url"):
        if preview and isinstance(value, str):
            encoded = f"<Base64 {os.path.basename(value)}>"
        elif not isinstance(value, str) or not os.path.isfile(value):
            raise ValueError("base64 source is not a media file")
        else:
            with open(value, "rb") as stream:
                encoded = base64.b64encode(stream.read()).decode()
        if transform == "data_url":
            mime = mimetypes.guess_type(str(value))[0] or "application/octet-stream"
            return f"data:{mime};base64,{encoded}"
        return encoded
    if transform == "file":
        if not isinstance(value, str) or (not preview and not os.path.isfile(value)):
            raise ValueError("file source is not ready")
        return value
    raise ValueError(f"unsupported mapping transform: {transform}")


def mapped_parts(
    event: Dict[str, Any], mappings: Iterable[Dict[str, Any]], *, preview: bool = False,
) -> Dict[str, Dict[str, Any]]:
    parts: Dict[str, Dict[str, Any]] = {
        "body": {}, "query": {}, "form": {}, "header": {}, "file": {},
    }
    for mapping in mappings:
        if not isinstance(mapping, dict):
            raise ValueError("each mapping entry must be an object")
        source = str(mapping.get("source", "")).strip()
        target = str(mapping.get("target", "")).strip()
        location = str(mapping.get("location", "body")).strip() or "body"
        required = bool(mapping.get("required", False))
        if location not in MAPPING_LOCATIONS:
            raise ValueError(f"unsupported mapping location: {location}")
        if not source or not target:
            if required:
                raise ValueError("required mapping source/target is empty")
            continue
        value = mapping.get("value") if source == "constant" else lookup(event, source)
        if value is MISSING:
            if required:
                if preview:
                    value = f"<required: {source}>"
                else:
                    raise ValueError(f"required mapping source is missing: {source}")
            else:
                continue
        value = coerce(value, str(mapping.get("type", "")))
        transform = str(mapping.get("transform", "")).strip()
        if transform in ("base64", "data_url", "file") and not source.startswith("media."):
            raise ValueError(f"file-content transform requires a media source: {source}")
        value = transform_value(value, transform, preview)
        if location == "file" and transform != "file":
            raise ValueError(f"file mapping {target} requires transform=file")
        set_path(parts[location], target, value)
    return parts


def response_path(root: Any, path: str) -> Any:
    current = root
    for part in (part for part in path.split(".") if part):
        if isinstance(current, dict) and part in current:
            current = current[part]
            continue
        if isinstance(current, list) and part.isdigit() and int(part) < len(current):
            current = current[int(part)]
            continue
        else:
            return MISSING
    return current
