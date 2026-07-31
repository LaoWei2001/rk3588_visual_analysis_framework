"""投递适配器目录、连接 Profile、请求预览和测试发送 API。"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict

import yaml
from fastapi import APIRouter, HTTPException

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
_SAFE_ID = re.compile(r"^[A-Za-z0-9._-]+$")

from services.data_dir import data_dir, migrate_app_data

router = APIRouter()
DEFAULT_UPLOAD_CONFIG: Dict[str, Any] = {"profiles": {}}
_ALLOWED_MEDIA = {"annotated_image", "raw_image", "video"}
_ALLOWED_TYPES = {"", "string", "number", "boolean", "json"}
_ALLOWED_SOURCE_ROOTS = {"event", "source", "fields", "media"}


def _app_dir(name: str) -> Path:
    app_dir = APPS_ROOT / name
    if not app_dir.exists():
        raise HTTPException(status_code=404, detail=f"App '{name}' not found")
    return app_dir


def _config_path(name: str) -> Path:
    app_dir = _app_dir(name)
    migrate_app_data(name, app_dir)
    return data_dir(name) / "upload_config.yaml"


def _catalog_path(name: str) -> Path:
    return _app_dir(name) / "services" / "upload" / "adapters" / "catalog.json"


def _contracts_dir(name: str) -> Path:
    app_dir = _app_dir(name)
    migrate_app_data(name, app_dir)
    return data_dir(name) / "contracts"


def _validated_contract(name: str, contract_id: str, body: Dict[str, Any]) -> Dict[str, Any]:
    if not _SAFE_ID.fullmatch(contract_id):
        raise HTTPException(status_code=400, detail="接口模板 ID 只能包含字母、数字、点、下划线和短横线")
    contract = dict(body)
    contract.pop("source_file", None)
    if str(contract.get("id", "")).strip() != contract_id:
        raise HTTPException(status_code=400, detail="接口模板 ID 与请求路径不一致")
    if not str(contract.get("label", "")).strip():
        raise HTTPException(status_code=400, detail="接口模板名称不能为空")

    catalog = {
        str(item.get("id", "")): item
        for item in _catalog(name)
        if isinstance(item, dict)
    }
    adapter = str(contract.get("adapter", "")).strip()
    adapter_def = catalog.get(adapter)
    if not isinstance(adapter_def, dict):
        raise HTTPException(status_code=400, detail=f"未知适配器: {adapter}")

    media = contract.get("media")
    if not isinstance(media, list) or any(str(item) not in _ALLOWED_MEDIA for item in media):
        raise HTTPException(status_code=400, detail="media 必须是有效的媒体数组")
    media = list(dict.fromkeys(str(item) for item in media))
    supported_media = {str(item) for item in adapter_def.get("supported_media", [])}
    if any(item not in supported_media for item in media):
        raise HTTPException(status_code=400, detail=f"适配器 {adapter} 不支持所选媒体")
    contract["media"] = media

    mappings = contract.get("mapping")
    if not isinstance(mappings, list):
        raise HTTPException(status_code=400, detail="mapping 必须是数组")
    allowed_transforms = {str(item) for item in adapter_def.get("transforms", [])}
    targets = set()
    normalized_mappings = []
    for index, raw in enumerate(mappings):
        if not isinstance(raw, dict):
            raise HTTPException(status_code=400, detail=f"第 {index + 1} 条字段映射必须是对象")
        mapping = dict(raw)
        source = str(mapping.get("source", "")).strip()
        target = str(mapping.get("target", "")).strip()
        if not source or not target:
            raise HTTPException(status_code=400, detail=f"第 {index + 1} 条字段映射缺少来源或远端字段")
        if target in targets:
            raise HTTPException(status_code=400, detail=f"远端字段重复: {target}")
        targets.add(target)
        if source == "constant":
            if "value" not in mapping:
                raise HTTPException(status_code=400, detail=f"第 {index + 1} 条固定值不能为空")
        else:
            root = source.split(".", 1)[0]
            if root not in _ALLOWED_SOURCE_ROOTS:
                raise HTTPException(status_code=400, detail=f"不支持的字段来源: {source}")
            if root == "media":
                variant = source.split(".", 1)[1] if "." in source else ""
                if variant not in media:
                    raise HTTPException(
                        status_code=400,
                        detail=f"字段来源 {source} 未在模板 media 中启用",
                    )
        value_type = str(mapping.get("type", "")).strip()
        if value_type not in _ALLOWED_TYPES:
            raise HTTPException(status_code=400, detail=f"不支持的字段类型: {value_type}")
        transform = str(mapping.get("transform", "")).strip()
        if transform not in allowed_transforms:
            raise HTTPException(status_code=400, detail=f"适配器 {adapter} 不支持转换: {transform}")
        if transform == "file":
            mapping["file_mode"] = str(mapping.get("file_mode", "single"))
            if mapping["file_mode"] not in ("single", "list"):
                raise HTTPException(status_code=400, detail="file_mode 只能是 single 或 list")
        else:
            mapping.pop("file_mode", None)
        mapping["source"] = source
        mapping["target"] = target
        normalized_mappings.append(mapping)
    contract["mapping"] = normalized_mappings

    request = contract.get("request")
    success = contract.get("success")
    if request is not None and not isinstance(request, dict):
        raise HTTPException(status_code=400, detail="request 必须是对象")
    if success is not None and not isinstance(success, dict):
        raise HTTPException(status_code=400, detail="success 必须是对象")
    if adapter == "http_json":
        request = dict(request or {})
        method = str(request.get("method", "POST")).upper()
        if method not in ("POST", "PUT", "PATCH"):
            raise HTTPException(status_code=400, detail="HTTP method 只能是 POST、PUT 或 PATCH")
        request["method"] = method
        contract["request"] = request
    else:
        contract.pop("request", None)
        contract.pop("success", None)
    return contract


def _contract_file(name: str, contract_id: str) -> Path:
    directory = _contracts_dir(name)
    if not directory.is_dir():
        raise HTTPException(status_code=500, detail="上传服务缺少 contracts 目录")
    for path in sorted(directory.glob("*.json")):
        try:
            contract = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if isinstance(contract, dict) and str(contract.get("id", "")).strip() == contract_id:
            return path
    return directory / f"{contract_id}.json"


def _catalog(name: str):
    path = _catalog_path(name)
    if not path.is_file():
        raise HTTPException(status_code=500, detail="上传服务缺少 adapters/catalog.json")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise HTTPException(status_code=500, detail=f"适配器目录无效: {exc}") from exc
    if not isinstance(data, list):
        raise HTTPException(status_code=500, detail="适配器目录必须是数组")
    return data


@router.get("/apps/{name}/delivery-adapters")
async def get_delivery_adapters(name: str):
    return {"adapters": _catalog(name)}


@router.get("/apps/{name}/report-contracts")
async def get_report_contracts(name: str):
    directory = _contracts_dir(name)
    if not directory.is_dir():
        raise HTTPException(status_code=500, detail="上传服务缺少 contracts 目录")
    contracts = []
    ids = set()
    try:
        paths = sorted(directory.glob("*.json"))
        for path in paths:
            contract = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(contract, dict):
                raise ValueError(f"{path.name} 必须是对象")
            contract_id = str(contract.get("id", "")).strip()
            if not contract_id or contract_id in ids:
                raise ValueError(f"{path.name} 的 id 为空或重复")
            if not isinstance(contract.get("media"), list) or not isinstance(contract.get("mapping"), list):
                raise ValueError(f"{path.name} 缺少 media/mapping 数组")
            ids.add(contract_id)
            contract["source_file"] = path.name
            contracts.append(contract)
    except (OSError, ValueError) as exc:
        raise HTTPException(status_code=500, detail=f"接口模板无效: {exc}") from exc
    return {"contracts": contracts}


@router.put("/apps/{name}/report-contracts/{contract_id}")
async def save_report_contract(name: str, contract_id: str, body: Dict[str, Any]):
    contract = _validated_contract(name, contract_id, body)
    path = _contract_file(name, contract_id)
    temporary = path.with_suffix(path.suffix + ".tmp")
    try:
        temporary.write_text(
            json.dumps(contract, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary, path)
    except OSError as exc:
        raise HTTPException(status_code=500, detail=f"保存接口模板失败: {exc}") from exc
    result = dict(contract)
    result["source_file"] = path.name
    return {"ok": True, "contract": result}


@router.get("/apps/{name}/upload-config")
async def get_upload_config(name: str):
    path = _config_path(name)
    if not path.exists():
        return DEFAULT_UPLOAD_CONFIG
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except Exception as exc:
        raise HTTPException(status_code=500, detail=f"解析 config.yaml 失败: {exc}") from exc
    profiles = data.get("profiles", {})
    return {"profiles": profiles if isinstance(profiles, dict) else {}}


@router.post("/apps/{name}/upload-config")
async def save_upload_config(name: str, body: Dict[str, Any]):
    profiles = body.get("profiles")
    if not isinstance(profiles, dict):
        raise HTTPException(status_code=400, detail="profiles 必须是对象")
    known = {str(item.get("id", "")) for item in _catalog(name) if isinstance(item, dict)}
    for profile_id, profile in profiles.items():
        if not _SAFE_ID.fullmatch(str(profile_id)) or not isinstance(profile, dict):
            raise HTTPException(status_code=400, detail=f"无效 Profile: {profile_id}")
        adapter = str(profile.get("adapter", ""))
        if adapter not in known:
            raise HTTPException(status_code=400, detail=f"未知适配器: {adapter}")

    path = _config_path(name)
    path.parent.mkdir(parents=True, exist_ok=True)
    text = yaml.safe_dump({"profiles": profiles}, allow_unicode=True, sort_keys=False)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(text, encoding="utf-8")
    os.replace(temporary, path)
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
    config = _config_path(name)
    if not tool.is_file() or not config.is_file():
        raise HTTPException(status_code=500, detail="上传服务尚未完整安装")
    contracts_dir = _contracts_dir(name)
    command = [
        sys.executable,
        str(tool),
        "--config",
        str(config),
        "--contracts-dir",
        str(contracts_dir),
        "--delivery-json",
        json.dumps(delivery, ensure_ascii=False),
    ]
    if event_id:
        override = os.environ.get("EVENT_STORE_DIR")
        if override:
            event_root = Path(override)
        else:
            app_dir = _app_dir(name)
            migrate_app_data(name, app_dir)
            event_root = data_dir(name) / "event_store"
        event_dir = event_root / event_id
        if not event_dir.is_dir():
            raise HTTPException(status_code=404, detail="本地事件不存在")
        command.extend(["--event-dir", str(event_dir)])
    if send:
        command.append("--send")
    try:
        completed = subprocess.run(
            command,
            cwd=str(tool.parent),
            capture_output=True,
            text=True,
            timeout=180,
            check=False,
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
