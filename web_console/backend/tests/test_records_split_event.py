#!/usr/bin/env python3
"""Records API 对标准事件 schema 的视图与重试请求测试。"""

import asyncio
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

BACKEND_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BACKEND_DIR))

from routers import records  # noqa: E402


class RecordsSplitEventTest(unittest.TestCase):
    @staticmethod
    def _write_record(root: str, event_id: str, trigger_unix_ms: int):
        event_dir = Path(root) / event_id
        event_dir.mkdir()
        (event_dir / "event.json").write_text(json.dumps({
            "schema_version": 3,
            "event": {
                "id": event_id,
                "type": "alarm",
                "created_unix_sec": trigger_unix_ms // 1000,
                "trigger_unix_ms": trigger_unix_ms,
            },
            "source": {"channel_id": 0},
            "data": {"fields": {}},
        }), encoding="utf-8")
        (event_dir / "media_state.json").write_text(json.dumps({
            "schema_version": 3, "status": "ready", "media": {},
        }), encoding="utf-8")
        (event_dir / "delivery_state.json").write_text(json.dumps({
            "schema_version": 3, "deliveries": [],
        }), encoding="utf-8")

    def test_media_statuses_are_exposed(self):
        with tempfile.TemporaryDirectory() as temporary:
            event_dir = Path(temporary) / "event-failed"
            event_dir.mkdir()
            (event_dir / "event.json").write_text(json.dumps({
                "schema_version": 3,
                "event": {"id": "event-failed", "type": "alarm"},
                "source": {"channel_id": 0, "parameters": {}},
                "data": {"fields": {}},
                "policy_snapshot": {},
            }), encoding="utf-8")
            (event_dir / "media_state.json").write_text(json.dumps({
                "schema_version": 3,
                "status": "failed",
                "media": {
                    "image": {
                        "status": "failed",
                        "error": "jpeg write failed",
                        "files": {"annotated_image": "", "raw_image": ""},
                    },
                    "video": {
                        "status": "generating",
                        "error": "",
                        "files": {"video": ""},
                    },
                },
            }), encoding="utf-8")
            (event_dir / "delivery_state.json").write_text(json.dumps({
                "schema_version": 3,
                "deliveries": [],
            }), encoding="utf-8")

            with patch.dict(os.environ, {"EVENT_STORE_DIR": temporary}):
                listing = asyncio.run(records.list_records("ignored"))

            record = listing["records"][0]
            self.assertEqual(record["state"], "failed")
            self.assertEqual(record["media_statuses"]["image"]["error"], "jpeg write failed")
            self.assertEqual(record["media_statuses"]["video"]["status"], "generating")

    def test_list_json_and_retry(self):
        with tempfile.TemporaryDirectory() as temporary:
            event_dir = Path(temporary) / "event-1"
            event_dir.mkdir()
            (event_dir / "event.json").write_text(json.dumps({
                "schema_version": 3,
                "event": {
                    "id": "event-1",
                    "type": "person_intrusion",
                    "message": "detected",
                    "created_unix_sec": 100,
                    "trigger_unix_ms": 123,
                    "trigger_count": 1,
                },
                "source": {"channel_id": 2, "parameters": {}},
                "data": {"fields": {"score": 0.9}},
                "policy_snapshot": {},
            }), encoding="utf-8")
            (event_dir / "media_state.json").write_text(json.dumps({
                "schema_version": 3,
                "status": "ready",
                "media": {},
            }), encoding="utf-8")
            (event_dir / "delivery_state.json").write_text(json.dumps({
                "schema_version": 3,
                "deliveries": [{
                    "id": "event-data",
                    "connection_id": "factory",
                    "contract_id": "server_event",
                    "contract_revision": "revision",
                    "media": [],
                    "status": "retry",
                }],
            }), encoding="utf-8")

            with patch.dict(os.environ, {"EVENT_STORE_DIR": temporary}):
                listing = asyncio.run(records.list_records("ignored"))
                self.assertEqual(listing["count"], 1)
                self.assertEqual(listing["records"][0]["event_type"], "person_intrusion")

                payload = asyncio.run(records.record_json("ignored", "event-1"))
                self.assertEqual(payload["source"]["channel_id"], 2)
                self.assertEqual(payload["fields"]["score"], 0.9)

                response = asyncio.run(records.retry_record("ignored", "event-1"))
                self.assertTrue(response["accepted"])
                self.assertTrue((event_dir / records.RETRY_REQUEST_FILE).is_file())

    def test_list_records_filters_by_trigger_time_before_limit(self):
        with tempfile.TemporaryDirectory() as temporary:
            self._write_record(temporary, "event-old", 1_000)
            self._write_record(temporary, "event-middle", 2_000)
            self._write_record(temporary, "event-new", 3_000)

            with patch.dict(os.environ, {"EVENT_STORE_DIR": temporary}):
                listing = asyncio.run(records.list_records(
                    "ignored", limit=1, start_unix_ms=1_500, end_unix_ms=3_000,
                ))

            self.assertEqual(listing["count"], 3)
            self.assertEqual(listing["filtered_count"], 2)
            self.assertEqual([item["id"] for item in listing["records"]], ["event-new"])


if __name__ == "__main__":
    unittest.main()
