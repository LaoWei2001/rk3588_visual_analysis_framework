"""Application-scoped delivery connections, contract templates and request preview APIs."""

import importlib.util
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Tuple

import yaml
from fastapi import APIRouter, HTTPException

from services.data_dir import data_dir, ensure_data_dir


APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
_SAFE_ID = re.compile(r"^[A-Za-z0-9._-]+$")
_ALLOWED_MEDIA = {"annotated_image", "raw_image", "video"}
_ALLOWED_TYPES = {"", "string", "number", "boolean", "json"}
_ALLOWED_SOURCE_ROOTS = {"event", "source", "fields", "media"}
_ALLOWED_LOCATIONS = {"body", "query", "form", "header", "file"}

router = APIRouter()


def _atomic_write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.{os.urandom(6).hex()}.tmp")
    try:
        with temporary.open("w", encoding="utf-8") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def _app_dir(name: str) -> Path:
    app_dir = APPS_ROOT / name
    if not app_dir.exists():
        raise HTTPException(status_code=404, detail=f"App '{name}' not found")
    return app_dir


def _connections_path(name: str) -> Path:
    return ensure_data_dir(name) / "connections.yaml"


def _templates_dir(name: str) -> Path:
    return _app_dir(name) / "report_templates"


def _custom_contracts_dir(name: str) -> Path:
    return ensure_data_dir(name) / "report_contracts"


def _revisions_dir(name: str) -> Path:
    return ensure_data_dir(name) / "contract_revisions"


def _catalog_path(name: str) -> Path:
    return _app_dir(name) / "services" / "upload" / "adapters" / "catalog.json"


def _contracts_module(name: str):
    path = _app_dir(name) / "services" / "upload" / "contracts.py"
    if not path.is_file():
        raise HTTPException(status_code=500, detail="程序包缺少版本化契约加载器")
    safe_name = re.sub(r"[^A-Za-z0-9_]", "_", name)
    module_name = f"app_contracts_{safe_name}_{path.stat().st_mtime_ns}"
    spec = importlib.util.spec_from_file_location(module_name, path)
    if spec is None or spec.loader is None:
        raise HTTPException(status_code=500, detail="无法加载程序包契约模块")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _resolved_contracts(name: str) -> Tuple[Dict[str, Dict[str, Any]], Dict[str, Dict[str, Any]]]:
    try:
        return _contracts_module(name).load_contracts(
            _templates_dir(name), _custom_contracts_dir(name), _revisions_dir(name),
        )
    except (OSError, ValueError) as exc:
        raise HTTPException(status_code=500, detail=f"接口契约目录无效: {exc}") from exc


def _catalog(name: str) -> List[Dict[str, Any]]:
    path = _catalog_path(name)
    if not path.is_file():
        raise HTTPException(status_code=500, detail="程序包缺少 adapters/catalog.json")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise HTTPException(status_code=500, detail=f"适配器目录无效: {exc}") from exc
    if not isinstance(data, list):
        raise HTTPException(status_code=500, detail="适配器目录必须是数组")
    return [item for item in data if isinstance(item, dict)]


def _logic_catalog(name: str) -> Dict[str, Dict[str, Any]]:
    path = _app_dir(name) / "logics.json"
    if not path.is_file():
        raise HTTPException(status_code=500, detail="程序包缺少 logics.json")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise HTTPException(status_code=500, detail=f"logics.json 无效: {exc}") from exc
    result: Dict[str, Dict[str, Any]] = {}
    for group in ("channel_logics", "global_logics"):
        values = data.get(group, []) if isinstance(data, dict) else []
        if not isinstance(values, list):
            raise HTTPException(status_code=500, detail=f"logics.json 缺少 {group}")
        for item in values:
            if isinstance(item, dict) and str(item.get("name", "")).strip():
                result[str(item["name"])] = item
    return result


def _validated_contract(name: str, contract_id: str, body: Dict[str, Any]) -> Dict[str, Any]:
    if not _SAFE_ID.fullmatch(contract_id):
        raise HTTPException(status_code=400, detail="契约 ID 只能包含字母、数字、点、下划线和短横线")
    contract = dict(body)
    for key in ("origin", "revision", "source_file", "_origin", "_revision", "_source_file"):
        contract.pop(key, None)
    if str(contract.get("id", "")).strip() != contract_id:
        raise HTTPException(status_code=400, detail="契约 ID 与请求路径不一致")
    if not str(contract.get("label", "")).strip():
        raise HTTPException(status_code=400, detail="契约名称不能为空")
    version = contract.get("version", 1)
    if not isinstance(version, int) or version < 1:
        raise HTTPException(status_code=400, detail="契约版本必须是正整数")
    contract["version"] = version

    adapters = {str(item.get("id", "")): item for item in _catalog(name)}
    adapter = str(contract.get("adapter", "")).strip()
    adapter_def = adapters.get(adapter)
    if not isinstance(adapter_def, dict):
        raise HTTPException(status_code=400, detail=f"未知适配器: {adapter}")

    owner_logic = str(contract.get("owner_logic", "")).strip()
    logics = _logic_catalog(name)
    logic = logics.get(owner_logic)
    if not owner_logic or not isinstance(logic, dict):
        raise HTTPException(status_code=400, detail=f"契约必须绑定有效 Logic: {owner_logic or '(empty)'}")
    contract["owner_logic"] = owner_logic
    declared_events = {
        str(item.get("id", "")) for item in logic.get("event_types", []) if isinstance(item, dict)
    }
    event_types = contract.get("event_types", [])
    if not isinstance(event_types, list) or not event_types:
        raise HTTPException(status_code=400, detail="契约至少绑定一个事件类型")
    unknown_events = [str(item) for item in event_types if str(item) not in declared_events]
    if unknown_events:
        raise HTTPException(status_code=400, detail=f"Logic 未声明事件类型: {', '.join(unknown_events)}")
    contract["event_types"] = list(dict.fromkeys(str(item) for item in event_types))

    media = contract.get("media")
    if not isinstance(media, list) or any(str(item) not in _ALLOWED_MEDIA for item in media):
        raise HTTPException(status_code=400, detail="media 必须是有效媒体数组")
    contract["media"] = list(dict.fromkeys(str(item) for item in media))
    supported_media = {str(item) for item in adapter_def.get("supported_media", [])}
    if any(item not in supported_media for item in contract["media"]):
        raise HTTPException(status_code=400, detail=f"适配器 {adapter} 不支持所选媒体")

    declared_fields = {
        str(item.get("key", "")) for item in logic.get("report_fields", []) if isinstance(item, dict)
    }
    allowed_transforms = {str(item) for item in adapter_def.get("transforms", [])}
    mappings = contract.get("mapping")
    if not isinstance(mappings, list) or not mappings:
        raise HTTPException(status_code=400, detail="mapping 至少包含一条字段映射")
    targets = set()
    adapter_targets = set()
    normalized = []
    for index, raw in enumerate(mappings):
        if not isinstance(raw, dict):
            raise HTTPException(status_code=400, detail=f"第 {index + 1} 条字段映射必须是对象")
        mapping = dict(raw)
        source = str(mapping.get("source", "")).strip()
        target = str(mapping.get("target", "")).strip()
        location = str(mapping.get("location", "body")).strip() or "body"
        if not source or not target:
            raise HTTPException(status_code=400, detail=f"第 {index + 1} 条映射缺少来源或目标")
        target_key = f"{location}:{target}"
        if target_key in targets:
            raise HTTPException(status_code=400, detail=f"远端字段重复: {target_key}")
        targets.add(target_key)
        if adapter == "dify_workflow" and target in adapter_targets:
            raise HTTPException(status_code=400, detail=f"Dify 输入变量重复: {target}")
        adapter_targets.add(target)
        if location not in _ALLOWED_LOCATIONS:
            raise HTTPException(status_code=400, detail=f"不支持的字段位置: {location}")
        if location != "body" and "." in target:
            raise HTTPException(status_code=400, detail=f"{location} 字段不支持嵌套目标: {target}")
        if adapter == "dify_workflow" and location not in ("body", "file"):
            raise HTTPException(status_code=400, detail="Dify 只支持 body/file 字段")
        if source == "constant":
            if "value" not in mapping:
                raise HTTPException(status_code=400, detail=f"第 {index + 1} 条固定值不能为空")
        else:
            root, _, tail = source.partition(".")
            if root not in _ALLOWED_SOURCE_ROOTS:
                raise HTTPException(status_code=400, detail=f"不支持的字段来源: {source}")
            if root == "fields" and tail and tail not in declared_fields:
                raise HTTPException(status_code=400, detail=f"{owner_logic} 未声明上报字段: {tail}")
            if root == "media" and tail not in contract["media"]:
                raise HTTPException(status_code=400, detail=f"媒体来源未启用: {source}")
        value_type = str(mapping.get("type", "")).strip()
        transform = str(mapping.get("transform", "")).strip()
        if value_type not in _ALLOWED_TYPES:
            raise HTTPException(status_code=400, detail=f"不支持的字段类型: {value_type}")
        if transform not in allowed_transforms:
            raise HTTPException(status_code=400, detail=f"适配器不支持转换: {transform}")
        if transform in ("base64", "data_url", "file") and not source.startswith("media."):
            raise HTTPException(status_code=400, detail=f"文件内容转换只能用于媒体来源: {source}")
        if location == "file" and transform != "file":
            raise HTTPException(status_code=400, detail="file 字段必须使用 file 转换")
        if transform == "file":
            mapping["file_mode"] = str(mapping.get("file_mode", "list"))
            if mapping["file_mode"] not in ("single", "list"):
                raise HTTPException(status_code=400, detail="file_mode 只能是 single 或 list")
        else:
            mapping.pop("file_mode", None)
        mapping.update({"source": source, "target": target, "location": location})
        normalized.append(mapping)
    contract["mapping"] = normalized

    if adapter == "http":
        request = contract.get("request")
        if not isinstance(request, dict):
            raise HTTPException(status_code=400, detail="HTTP 契约必须声明 request")
        method = str(request.get("method", "POST")).upper()
        path = str(request.get("path", "")).strip()
        encoding = str(request.get("encoding", "json")).strip()
        if method not in ("POST", "PUT", "PATCH") or not path or encoding not in ("json", "form", "multipart"):
            raise HTTPException(status_code=400, detail="HTTP request 的 method/path/encoding 无效")
        contract["request"] = {**request, "method": method, "path": path, "encoding": encoding}
        locations = {str(item.get("location", "body")) for item in normalized}
        if encoding == "json" and locations & {"form", "file"}:
            raise HTTPException(status_code=400, detail="JSON 请求不能包含 form/file 字段")
        if encoding == "form" and locations & {"body", "file"}:
            raise HTTPException(status_code=400, detail="Form 请求只能使用 form/query/header 字段")
        if encoding == "multipart" and "body" in locations:
            json_part = str(request.get("json_part", "")).strip()
            if not json_part:
                raise HTTPException(status_code=400, detail="Multipart 的 body 字段必须声明 JSON Part 名称")
            occupied = {
                str(item.get("target", "")) for item in normalized
                if item.get("location") in ("form", "file")
            }
            if json_part in occupied:
                raise HTTPException(status_code=400, detail="JSON Part 名称不能与 form/file 字段重名")
            contract["request"]["json_part"] = json_part
        if encoding == "multipart" and not locations & {"body", "file"}:
            raise HTTPException(status_code=400, detail="Multipart 至少需要一个文件或 JSON body")
    else:
        contract.pop("request", None)
        contract.pop("success", None)
    return contract


@router.get("/apps/{name}/delivery-adapters")
async def get_delivery_adapters(name: str):
    return {"adapters": _catalog(name)}


@router.get("/apps/{name}/connections")
async def get_connections(name: str):
    path = _connections_path(name)
    if not path.is_file():
        return {"connections": {}}
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except Exception as exc:
        raise HTTPException(status_code=500, detail=f"connections.yaml 无效: {exc}") from exc
    connections = data.get("connections", {})
    return {"connections": connections if isinstance(connections, dict) else {}}


@router.put("/apps/{name}/connections")
async def save_connections(name: str, body: Dict[str, Any]):
    connections = body.get("connections")
    if not isinstance(connections, dict):
        raise HTTPException(status_code=400, detail="connections 必须是对象")
    known = {str(item.get("id", "")): item for item in _catalog(name)}
    for connection_id, connection in connections.items():
        if not _SAFE_ID.fullmatch(str(connection_id)) or not isinstance(connection, dict):
            raise HTTPException(status_code=400, detail=f"无效连接: {connection_id}")
        adapter_id = str(connection.get("adapter", ""))
        adapter = known.get(adapter_id)
        if not isinstance(adapter, dict):
            raise HTTPException(status_code=400, detail=f"连接 {connection_id} 使用未知适配器")
        for field in adapter.get("connection_fields", []):
            if not isinstance(field, dict) or not field.get("required"):
                continue
            key = str(field.get("key", ""))
            value = connection.get(key)
            if value is None or (isinstance(value, str) and not value.strip()):
                raise HTTPException(status_code=400, detail=f"连接 {connection_id} 缺少必填项: {key}")
    path = _connections_path(name)
    _atomic_write_text(
        path, yaml.safe_dump({"connections": connections}, allow_unicode=True, sort_keys=False),
    )
    return {"ok": True, "app": name}


@router.get("/apps/{name}/report-contracts")
async def get_report_contracts(name: str):
    active, _revisions = _resolved_contracts(name)
    contracts = []
    for contract in active.values():
        item = {key: value for key, value in contract.items() if not key.startswith("_")}
        item["origin"] = contract.get("_origin")
        item["revision"] = contract.get("_revision")
        contracts.append(item)
    return {"contracts": sorted(contracts, key=lambda item: str(item.get("id", "")))}


@router.put("/apps/{name}/report-contracts/{contract_id}")
async def save_report_contract(name: str, contract_id: str, body: Dict[str, Any]):
    contract = _validated_contract(name, contract_id, body)
    directory = _custom_contracts_dir(name)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{contract_id}.json"
    _atomic_write_text(path, json.dumps(contract, ensure_ascii=False, indent=2) + "\n")
    active, _revisions = _resolved_contracts(name)
    saved = active[contract_id]
    result = {key: value for key, value in saved.items() if not key.startswith("_")}
    result.update({"origin": "custom", "revision": saved["_revision"]})
    return {"ok": True, "contract": result}


@router.delete("/apps/{name}/report-contracts/{contract_id}")
async def delete_report_contract(name: str, contract_id: str):
    if not _SAFE_ID.fullmatch(contract_id):
        raise HTTPException(status_code=400, detail="契约 ID 非法")
    path = _custom_contracts_dir(name) / f"{contract_id}.json"
    if not path.is_file():
        raise HTTPException(status_code=404, detail="只能删除应用自定义契约")
    path.unlink()
    return {"ok": True}


@router.post("/apps/{name}/delivery-preview")
async def preview_or_test_delivery(name: str, body: Dict[str, Any]):
    app_dir = _app_dir(name)
    delivery = body.get("delivery")
    if not isinstance(delivery, dict):
        raise HTTPException(status_code=400, detail="delivery 必须是对象")
    send = bool(body.get("send", False))
    event_id = str(body.get("event_id", "")).strip()
    if event_id and not _SAFE_ID.fullmatch(event_id):
        raise HTTPException(status_code=400, detail="事件 ID 非法")
    if send and not event_id:
        raise HTTPException(status_code=400, detail="测试发送必须选择一条本地事件")
    tool = app_dir / "services" / "upload" / "delivery_tool.py"
    if not tool.is_file():
        raise HTTPException(status_code=500, detail="程序包缺少投递工具")
    command = [
        sys.executable, str(tool),
        "--connections", str(_connections_path(name)),
        "--templates-dir", str(_templates_dir(name)),
        "--custom-contracts-dir", str(_custom_contracts_dir(name)),
        "--revisions-dir", str(_revisions_dir(name)),
        "--delivery-json", json.dumps(delivery, ensure_ascii=False),
    ]
    if event_id:
        event_dir = data_dir(name) / "event_store" / event_id
        if not event_dir.is_dir():
            raise HTTPException(status_code=404, detail="本地事件不存在")
        command.extend(["--event-dir", str(event_dir)])
    if send:
        command.append("--send")
    try:
        completed = subprocess.run(
            command, cwd=str(tool.parent), capture_output=True, text=True,
            timeout=180, check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise HTTPException(status_code=500, detail=f"执行投递工具失败: {exc}") from exc
    if completed.returncode != 0:
        detail = completed.stderr.strip().splitlines()[-1] if completed.stderr.strip() else "未知错误"
        raise HTTPException(status_code=400, detail=detail)
    try:
        return json.loads(completed.stdout)
    except ValueError as exc:
        raise HTTPException(status_code=500, detail="投递工具返回了无效 JSON") from exc
