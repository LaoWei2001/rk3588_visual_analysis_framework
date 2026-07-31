#!/usr/bin/env python3
"""接口模板目录 API 测试。"""

import asyncio
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

BACKEND_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BACKEND_DIR))

from routers import upload_config  # noqa: E402


class ReportContractsTest(unittest.TestCase):
    def test_contract_catalog_is_exposed(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "demo"
            contracts = app / "services" / "upload" / "contracts"
            contracts.mkdir(parents=True)
            (contracts / "server.json").write_text(json.dumps({
                "id": "server_event",
                "label": "服务器事件",
                "adapter": "http_json",
                "media": ["annotated_image"],
                "mapping": [
                    {"source": "constant", "target": "source", "value": "JNU"},
                ],
            }), encoding="utf-8")

            with patch.object(upload_config, "APPS_ROOT", Path(temporary)):
                result = asyncio.run(upload_config.get_report_contracts("demo"))

            self.assertEqual(result["contracts"][0]["id"], "server_event")
            self.assertEqual(result["contracts"][0]["source_file"], "server.json")

    def test_contract_can_be_created_and_updated(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "demo"
            contracts = app / "services" / "upload" / "contracts"
            adapters = app / "services" / "upload" / "adapters"
            contracts.mkdir(parents=True)
            adapters.mkdir(parents=True)
            (adapters / "catalog.json").write_text(json.dumps([{
                "id": "http_json",
                "supported_media": ["annotated_image", "raw_image", "video"],
                "transforms": ["", "base64", "json_string", "file"],
            }]), encoding="utf-8")
            body = {
                "id": "periodic_snapshot_http",
                "label": "周期截图接口",
                "adapter": "http_json",
                "media": ["annotated_image"],
                "request": {"method": "post"},
                "mapping": [
                    {"source": "constant", "target": "source", "value": "JNU"},
                    {"source": "fields.display_number", "target": "number", "type": "number"},
                    {
                        "source": "media.annotated_image",
                        "target": "image",
                        "transform": "base64",
                    },
                ],
                "success": {"http_status": [200]},
            }

            with patch.object(upload_config, "APPS_ROOT", Path(temporary)):
                created = asyncio.run(upload_config.save_report_contract(
                    "demo", "periodic_snapshot_http", body,
                ))
                listed = asyncio.run(upload_config.get_report_contracts("demo"))

            self.assertTrue(created["ok"])
            self.assertEqual(created["contract"]["request"]["method"], "POST")
            self.assertEqual(listed["contracts"][0]["mapping"][1]["source"],
                             "fields.display_number")
            saved = json.loads((contracts / "periodic_snapshot_http.json").read_text())
            self.assertNotIn("source_file", saved)

    def test_contract_rejects_media_source_not_enabled(self):
        with tempfile.TemporaryDirectory() as temporary:
            app = Path(temporary) / "demo"
            contracts = app / "services" / "upload" / "contracts"
            adapters = app / "services" / "upload" / "adapters"
            contracts.mkdir(parents=True)
            adapters.mkdir(parents=True)
            (adapters / "catalog.json").write_text(json.dumps([{
                "id": "http_json",
                "supported_media": ["annotated_image"],
                "transforms": ["", "base64"],
            }]), encoding="utf-8")
            body = {
                "id": "bad",
                "label": "错误模板",
                "adapter": "http_json",
                "media": [],
                "mapping": [{
                    "source": "media.annotated_image",
                    "target": "image",
                    "transform": "base64",
                }],
            }

            with patch.object(upload_config, "APPS_ROOT", Path(temporary)):
                with self.assertRaises(upload_config.HTTPException) as raised:
                    asyncio.run(upload_config.save_report_contract("demo", "bad", body))

            self.assertEqual(raised.exception.status_code, 400)
            self.assertIn("未在模板 media 中启用", raised.exception.detail)


if __name__ == "__main__":
    unittest.main()
