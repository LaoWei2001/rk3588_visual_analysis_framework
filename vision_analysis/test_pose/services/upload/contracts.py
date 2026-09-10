"""Versioned report-contract catalog.

Package templates are immutable application capabilities. User-created contracts live in
the application's persistent data directory. Every resolved contract is archived by its
content revision so queued events always use the contract selected when they were created.
"""

import hashlib
import json
import os
from pathlib import Path
from typing import Any, Dict, Iterable, Optional, Tuple


ALLOWED_MEDIA = {"annotated_image", "raw_image", "video"}
CONTRACT_KEYS = ("adapter", "media", "mapping", "request", "success")


def _canonical_contract(contract: Dict[str, Any]) -> Dict[str, Any]:
    return {key: value for key, value in contract.items() if not str(key).startswith("_")}


def contract_revision(contract: Dict[str, Any]) -> str:
    payload = json.dumps(
        _canonical_contract(contract), ensure_ascii=False, sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _validate_contract(path: Path, raw: Any) -> Dict[str, Any]:
    if not isinstance(raw, dict):
        raise ValueError(f"report contract {path.name} must be an object")
    contract = dict(raw)
    contract_id = str(contract.get("id", "")).strip()
    adapter = str(contract.get("adapter", "")).strip()
    version = contract.get("version")
    owner_logic = contract.get("owner_logic")
    event_types = contract.get("event_types", [])
    media = contract.get("media")
    mapping = contract.get("mapping")
    if not contract_id or not adapter:
        raise ValueError(f"report contract {path.name} requires id and adapter")
    if not isinstance(version, int) or version < 1:
        raise ValueError(f"report contract {contract_id} requires positive integer version")
    if owner_logic is not None and (not isinstance(owner_logic, str) or not owner_logic.strip()):
        raise ValueError(f"report contract {contract_id} has invalid owner_logic")
    if not isinstance(event_types, list) or any(not isinstance(item, str) for item in event_types):
        raise ValueError(f"report contract {contract_id} has invalid event_types")
    if not isinstance(media, list) or any(str(item) not in ALLOWED_MEDIA for item in media):
        raise ValueError(f"report contract {contract_id} has invalid media")
    if not isinstance(mapping, list):
        raise ValueError(f"report contract {contract_id} mapping must be an array")
    contract["id"] = contract_id
    contract["adapter"] = adapter
    contract["event_types"] = list(dict.fromkeys(event_types))
    contract["media"] = list(dict.fromkeys(str(item) for item in media))
    return contract


def _load_directory(directory: Optional[Path], origin: str) -> Dict[str, Dict[str, Any]]:
    contracts: Dict[str, Dict[str, Any]] = {}
    if directory is None or not directory.is_dir():
        return contracts
    for path in sorted(directory.rglob("*.json")):
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise ValueError(f"invalid report contract {path}: {exc}") from exc
        contract = _validate_contract(path, raw)
        contract_id = contract["id"]
        if contract_id in contracts:
            raise ValueError(f"duplicate report contract id in {origin}: {contract_id}")
        contract["_origin"] = origin
        contract["_source_file"] = str(path)
        contract["_revision"] = contract_revision(contract)
        contracts[contract_id] = contract
    return contracts


def _archive_contracts(
    contracts: Iterable[Dict[str, Any]], revisions_dir: Optional[Path],
) -> Dict[str, Dict[str, Any]]:
    revisions: Dict[str, Dict[str, Any]] = {}
    if revisions_dir is not None:
        revisions_dir.mkdir(parents=True, exist_ok=True)
    for contract in contracts:
        revision = str(contract.get("_revision") or contract_revision(contract))
        archived = dict(contract)
        archived["_revision"] = revision
        revisions[revision] = archived
        if revisions_dir is None:
            continue
        destination = revisions_dir / f"{revision}.json"
        if destination.exists():
            continue
        temporary = destination.with_name(
            f".{destination.name}.{os.getpid()}.{os.urandom(6).hex()}.tmp"
        )
        try:
            temporary.write_text(
                json.dumps(_canonical_contract(contract), ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            os.replace(temporary, destination)
        finally:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
    return revisions


def load_contracts(
    templates_dir: Optional[Path],
    custom_dir: Optional[Path],
    revisions_dir: Optional[Path] = None,
) -> Tuple[Dict[str, Dict[str, Any]], Dict[str, Dict[str, Any]]]:
    """Return active contracts by id and immutable contracts by revision."""
    templates = _load_directory(templates_dir, "package")
    custom = _load_directory(custom_dir, "custom")
    # 程序包模板是当前源码能力；历史 Web 版本留下的同键 custom 文件不能遮蔽新部署内容。
    # custom 仍可提供程序包中不存在的新模板。两边的 revision 都会继续归档，旧事件可重放。
    active = {**custom, **templates}
    revisions = _archive_contracts(
        [*templates.values(), *custom.values()], revisions_dir,
    )
    if revisions_dir is not None and revisions_dir.is_dir():
        for path in sorted(revisions_dir.glob("*.json")):
            try:
                contract = _validate_contract(path, json.loads(path.read_text(encoding="utf-8")))
            except (OSError, ValueError) as exc:
                raise ValueError(f"invalid archived contract {path}: {exc}") from exc
            revision = contract_revision(contract)
            if path.stem != revision:
                raise ValueError(f"archived contract filename does not match content: {path.name}")
            contract["_origin"] = "revision"
            contract["_source_file"] = str(path)
            contract["_revision"] = revision
            revisions[revision] = contract
    return active, revisions


def apply_contract(
    delivery: Dict[str, Any],
    contracts_by_revision: Dict[str, Dict[str, Any]],
) -> Dict[str, Any]:
    contract_id = str(delivery.get("contract_id", "")).strip()
    revision = str(delivery.get("contract_revision", "")).strip()
    if not contract_id or not revision:
        raise ValueError("delivery requires contract_id and contract_revision")
    contract = contracts_by_revision.get(revision)
    if not isinstance(contract, dict) or contract.get("id") != contract_id:
        raise ValueError(f"report contract revision not found: {contract_id}@{revision[:12]}")
    configured_media = delivery.get("media")
    if set(configured_media or []) != set(contract.get("media") or []):
        raise ValueError(
            f"delivery media plan does not match contract {contract_id}@{revision[:12]}"
        )
    effective = dict(delivery)
    for key in CONTRACT_KEYS:
        if key in contract:
            effective[key] = contract[key]
    return effective
