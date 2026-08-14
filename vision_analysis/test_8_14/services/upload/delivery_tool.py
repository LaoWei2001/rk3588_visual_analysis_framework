#!/usr/bin/env python3
"""供 Web 控制台调用的投递预览/测试工具；不修改发件箱状态。"""

import argparse
import json
import os
from pathlib import Path
from typing import Any, Dict

import yaml

from adapters import create_adapter
from contracts import apply_contract, load_contracts
from event_outbox import EventOutboxForwarder


def sample_event() -> Dict[str, Any]:
    return {
        "schema_version": 3,
        "event": {
            "id": "preview-event",
            "type": "sample_event",
            "message": "接口映射预览",
            "trigger_unix_ms": 1700000000000,
            "snap_time": "2023-11-14 22:13:20",
            "end_time": "2023-11-14 22:13:20",
            "trigger_count": 1,
        },
        "source": {
            "channel_id": 0,
            "parameters": {"device_id": "camera-01"},
        },
        "fields": {
            "score": 0.95,
            "person_count": 2,
            "region": "demo-zone",
        },
        "media": {
            "annotated_image": "/preview/annotated.jpg",
            "raw_image": "/preview/raw.jpg",
            "video": "/preview/clip.mp4",
        },
        "media_state": {},
        "policy": {},
        "deliveries": [],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", required=True)
    parser.add_argument("--delivery-json", required=True)
    parser.add_argument("--event-dir", default="")
    parser.add_argument("--contracts-dir", default="")
    parser.add_argument("--send", action="store_true")
    args = parser.parse_args()

    with open(args.config, "r", encoding="utf-8") as stream:
        config = yaml.safe_load(stream) or {}
    delivery = json.loads(args.delivery_json)
    if not isinstance(delivery, dict):
        raise ValueError("delivery must be an object")
    contracts_root = Path(args.contracts_dir) if args.contracts_dir else None
    delivery = apply_contract(delivery, load_contracts(contracts_root))
    event = EventOutboxForwarder._read(args.event_dir) if args.event_dir else sample_event()

    profile_id = str(delivery.get("profile_id", "")).strip()
    profiles = config.get("profiles", {})
    profile = profiles.get(profile_id) if isinstance(profiles, dict) else None
    if not isinstance(profile, dict):
        raise ValueError(f"profile {profile_id or '(empty)'} not found")
    adapter_id = str(delivery.get("adapter", "")).strip()
    if str(profile.get("adapter", "")).strip() != adapter_id:
        raise ValueError(f"profile {profile_id} does not use adapter {adapter_id}")
    adapter = create_adapter(adapter_id, profile)

    output: Dict[str, Any] = {"preview": adapter.preview(args.event_dir, event, delivery)}
    if args.send:
        if not args.event_dir or not os.path.isdir(args.event_dir):
            raise ValueError("test send requires an existing event")
        result = adapter.send(args.event_dir, event, delivery)
        output["test"] = {
            "ok": result.ok,
            "detail": result.detail,
            "status_code": result.status_code,
            "response": result.response,
        }
    print(json.dumps(output, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
