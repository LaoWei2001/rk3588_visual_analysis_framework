#!/usr/bin/env python3
"""Preview or test one versioned delivery without mutating outbox state."""

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
            "id": "preview-event", "type": "sample_event", "message": "接口映射预览",
            "trigger_unix_ms": 1700000000000, "snap_time": "2023-11-14 22:13:20",
            "end_time": "2023-11-14 22:13:20", "trigger_count": 1,
        },
        "source": {"channel_id": 0, "parameters": {"device_id": "camera-01"}},
        "fields": {"score": 0.95, "person_count": 2, "region": "demo-zone"},
        "media": {
            "annotated_image": "/preview/annotated.jpg",
            "raw_image": "/preview/raw.jpg", "video": "/preview/clip.mp4",
        },
        "media_state": {}, "policy": {}, "deliveries": [],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--connections", required=True)
    parser.add_argument("--templates-dir", required=True)
    parser.add_argument("--custom-contracts-dir", required=True)
    parser.add_argument("--revisions-dir", required=True)
    parser.add_argument("--delivery-json", required=True)
    parser.add_argument("--event-dir", default="")
    parser.add_argument("--send", action="store_true")
    args = parser.parse_args()

    connections_path = Path(args.connections)
    if connections_path.is_file():
        with connections_path.open("r", encoding="utf-8") as stream:
            config = yaml.safe_load(stream) or {}
    else:
        config = {}
    connections = config.get("connections", {})
    if not isinstance(connections, dict):
        raise ValueError("connections.yaml connections must be an object")
    _active, revisions = load_contracts(
        Path(args.templates_dir), Path(args.custom_contracts_dir), Path(args.revisions_dir),
    )
    delivery = json.loads(args.delivery_json)
    if not isinstance(delivery, dict):
        raise ValueError("delivery must be an object")
    delivery = apply_contract(delivery, revisions)
    event = EventOutboxForwarder._read(args.event_dir) if args.event_dir else sample_event()

    connection_id = str(delivery.get("connection_id", "")).strip()
    connection = connections.get(connection_id)
    if not isinstance(connection, dict):
        raise ValueError(f"connection {connection_id or '(empty)'} not found")
    adapter_id = str(delivery.get("adapter", "")).strip()
    if str(connection.get("adapter", "")).strip() != adapter_id:
        raise ValueError(f"connection {connection_id} does not use adapter {adapter_id}")
    adapter = create_adapter(adapter_id, connection)

    output: Dict[str, Any] = {"preview": adapter.preview(args.event_dir, event, delivery)}
    if args.send:
        if not args.event_dir or not os.path.isdir(args.event_dir):
            raise ValueError("test send requires an existing event")
        result = adapter.send(args.event_dir, event, delivery)
        output["test"] = {
            "ok": result.ok, "detail": result.detail, "status_code": result.status_code,
            "response": result.response,
        }
    print(json.dumps(output, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
