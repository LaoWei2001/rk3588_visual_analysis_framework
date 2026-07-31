"""可复用远端接口模板。模板是 mapping、媒体需求和成功条件的运行时权威。"""

import json
from pathlib import Path
from typing import Any, Dict, Optional

ALLOWED_MEDIA = {"annotated_image", "raw_image", "video"}
CONTRACT_KEYS = ("adapter", "media", "mapping", "request", "success")


def load_contracts(directory: Optional[Path] = None) -> Dict[str, Dict[str, Any]]:
    root = directory or Path(__file__).with_name("contracts")
    contracts: Dict[str, Dict[str, Any]] = {}
    if not root.is_dir():
        raise ValueError(f"report contracts directory not found: {root}")
    for path in sorted(root.glob("*.json")):
        try:
            contract = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise ValueError(f"invalid report contract {path.name}: {exc}") from exc
        if not isinstance(contract, dict):
            raise ValueError(f"report contract {path.name} must be an object")
        contract_id = str(contract.get("id", "")).strip()
        adapter = str(contract.get("adapter", "")).strip()
        media = contract.get("media")
        mapping = contract.get("mapping")
        if not contract_id or not adapter:
            raise ValueError(f"report contract {path.name} requires id and adapter")
        if contract_id in contracts:
            raise ValueError(f"duplicate report contract id: {contract_id}")
        if not isinstance(media, list) or any(str(item) not in ALLOWED_MEDIA for item in media):
            raise ValueError(f"report contract {contract_id} has invalid media")
        if not isinstance(mapping, list):
            raise ValueError(f"report contract {contract_id} mapping must be an array")
        contract["_source_file"] = path.name
        contracts[contract_id] = contract
    return contracts


def apply_contract(
    delivery: Dict[str, Any],
    contracts: Dict[str, Dict[str, Any]],
) -> Dict[str, Any]:
    contract_id = str(delivery.get("contract_id", "")).strip()
    if not contract_id:
        raise ValueError("delivery contract_id is empty")
    contract = contracts.get(contract_id)
    if not isinstance(contract, dict):
        raise ValueError(f"report contract {contract_id} not found")
    configured_media = delivery.get("media")
    if set(configured_media or []) != set(contract.get("media") or []):
        raise ValueError(
            f"delivery media snapshot does not match contract {contract_id}; "
            "reselect the interface template and save the channel config"
        )
    effective = dict(delivery)
    for key in CONTRACT_KEYS:
        if key in contract:
            effective[key] = contract[key]
    return effective
