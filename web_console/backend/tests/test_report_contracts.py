#!/usr/bin/env python3
"""Application-scoped connection and contract API tests."""

import asyncio
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

BACKEND_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = BACKEND_DIR.parents[1]
sys.path.insert(0, str(BACKEND_DIR))

from routers import delivery_config
from services import data_dir as app_data_dir


class DeliveryConfigurationTest(unittest.TestCase):
    def fixture(self, root: Path) -> Path:
        app = root / "demo"
        upload = app / "services" / "upload"
        adapters = upload / "adapters"
        adapters.mkdir(parents=True)
        shutil.copy2(REPO_ROOT / "service/upload/contracts.py", upload / "contracts.py")
        shutil.copy2(REPO_ROOT / "service/upload/adapters/catalog.json", adapters / "catalog.json")
        (app / "report_templates").mkdir()
        (app / "logics.json").write_text(json.dumps({
            "channel_logics": [{
                "name": "logic_demo",
                "event_types": [{"id": "demo"}],
                "report_fields": [{"key": "score", "type": "number"}],
            }],
            "global_logics": [],
        }), encoding="utf-8")
        return app

    def test_connections_are_scoped_to_selected_application(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.fixture(root)
            with (
                patch.object(delivery_config, "APPS_ROOT", root),
                patch.object(app_data_dir, "APPS_ROOT", root),
            ):
                asyncio.run(delivery_config.save_connections("demo", {
                    "connections": {"server": {"adapter": "http", "base_url": "http://x"}},
                }))
                result = asyncio.run(delivery_config.get_connections("demo"))
            self.assertEqual(result["connections"]["server"]["adapter"], "http")

    def test_custom_contract_is_validated_against_logic_schema(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.fixture(root)
            body = {
                "id": "logic_demo.http", "version": 1, "label": "demo",
                "owner_logic": "logic_demo", "event_types": ["demo"],
                "adapter": "http", "media": [],
                "request": {"method": "POST", "path": "/events", "encoding": "json"},
                "mapping": [{
                    "source": "fields.score", "target": "score", "location": "body",
                    "type": "number", "required": True,
                }],
                "revision": "",
            }
            with (
                patch.object(delivery_config, "APPS_ROOT", root),
                patch.object(app_data_dir, "APPS_ROOT", root),
            ):
                saved = asyncio.run(delivery_config.save_report_contract("demo", body["id"], body))
                listed = asyncio.run(delivery_config.get_report_contracts("demo"))
            self.assertEqual(saved["contract"]["origin"], "custom")
            self.assertTrue(saved["contract"]["revision"])
            self.assertEqual(listed["contracts"][0]["owner_logic"], "logic_demo")
            self.assertFalse(listed["contracts"][0]["package_template"])

    def test_saving_existing_template_overwrites_active_content_without_duplicate_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            app = self.fixture(root)
            body = {
                "id": "logic_demo.http", "version": 1, "label": "程序包模板",
                "owner_logic": "logic_demo", "event_types": ["demo"],
                "adapter": "http", "media": [],
                "request": {"method": "POST", "path": "/events", "encoding": "json"},
                "mapping": [{
                    "source": "fields.score", "target": "score", "location": "body",
                    "type": "number", "required": True,
                }],
            }
            (app / "report_templates" / "package.json").write_text(
                json.dumps(body), encoding="utf-8",
            )
            legacy_dir = root / ".data" / "demo" / "report_contracts"
            legacy_dir.mkdir(parents=True)
            (legacy_dir / "logic_demo.http.json").write_text(
                json.dumps({**body, "version": 9, "label": "历史隐藏覆盖"}),
                encoding="utf-8",
            )
            with (
                patch.object(delivery_config, "APPS_ROOT", root),
                patch.object(app_data_dir, "APPS_ROOT", root),
            ):
                first = {**body, "version": 2, "label": "第一次修改"}
                second = {**body, "version": 3, "label": "最终模板名称"}
                asyncio.run(delivery_config.save_report_contract("demo", body["id"], first))
                saved = asyncio.run(
                    delivery_config.save_report_contract("demo", body["id"], second),
                )
                listed = asyncio.run(delivery_config.get_report_contracts("demo"))

            package_path = app / "report_templates" / "package.json"
            custom_path = root / ".data" / "demo" / "report_contracts" / "logic_demo.http.json"
            self.assertEqual(json.loads(package_path.read_text(encoding="utf-8"))["label"], "最终模板名称")
            self.assertFalse(custom_path.exists())
            self.assertEqual(saved["contract"]["label"], "最终模板名称")
            self.assertEqual(saved["contract"]["origin"], "package")
            self.assertEqual(len(listed["contracts"]), 1)
            self.assertEqual(listed["contracts"][0]["label"], "最终模板名称")
            self.assertTrue(listed["contracts"][0]["package_template"])


if __name__ == "__main__":
    unittest.main()
