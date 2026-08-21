import json
import tempfile
import unittest
from pathlib import Path

from adapters.dify_workflow import DifyWorkflowAdapter
from adapters.http import HttpAdapter
from adapters.mapping import mapped_parts
from contracts import apply_contract, contract_revision, load_contracts


def contract(contract_id: str = "logic_demo.http"):
    return {
        "id": contract_id,
        "version": 1,
        "label": "demo",
        "owner_logic": "logic_demo",
        "event_types": ["demo"],
        "adapter": "http",
        "media": ["annotated_image"],
        "request": {"method": "POST", "path": "/events", "encoding": "multipart"},
        "mapping": [
            {"source": "event.type", "target": "eventType", "location": "form"},
            {
                "source": "media.annotated_image", "target": "image",
                "location": "file", "transform": "file", "required": True,
            },
        ],
    }


class ContractCatalogTest(unittest.TestCase):
    def test_custom_contract_shadows_package_and_revisions_are_archived(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            templates = root / "templates"
            custom = root / "custom"
            revisions = root / "revisions"
            templates.mkdir()
            custom.mkdir()
            package = contract()
            override = {**package, "version": 2, "label": "custom"}
            (templates / "package.json").write_text(json.dumps(package), encoding="utf-8")
            (custom / "custom.json").write_text(json.dumps(override), encoding="utf-8")

            active, archived = load_contracts(templates, custom, revisions)

            selected = active[package["id"]]
            self.assertEqual(selected["_origin"], "custom")
            self.assertEqual(selected["version"], 2)
            self.assertIn(contract_revision(package), archived)
            self.assertIn(selected["_revision"], archived)
            self.assertEqual(len(archived), 2)
            self.assertTrue((revisions / f"{selected['_revision']}.json").is_file())

    def test_delivery_is_resolved_by_immutable_revision(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            templates = root / "templates"
            templates.mkdir()
            value = contract()
            (templates / "contract.json").write_text(json.dumps(value), encoding="utf-8")
            active, archived = load_contracts(templates, None, root / "revisions")
            selected = active[value["id"]]
            delivery = {
                "contract_id": value["id"], "contract_revision": selected["_revision"],
                "connection_id": "server", "media": ["annotated_image"],
            }
            effective = apply_contract(delivery, archived)
            self.assertEqual(effective["adapter"], "http")


class MappingAndAdapterTest(unittest.TestCase):
    EVENT = {
        "event": {"id": "e1", "type": "demo"},
        "source": {"channel_id": 0},
        "fields": {"score": 0.9},
        "media": {"annotated_image": "/tmp/image.jpg"},
    }

    def test_mapping_splits_request_locations(self):
        parts = mapped_parts(self.EVENT, [
            {"source": "event.type", "target": "kind", "location": "query"},
            {"source": "fields.score", "target": "score", "location": "body"},
        ], preview=True)
        self.assertEqual(parts["query"]["kind"], "demo")
        self.assertEqual(parts["body"]["score"], 0.9)

    def test_http_preview_combines_base_url_and_contract_path(self):
        adapter = HttpAdapter({"base_url": "http://server.example/base", "headers": {}})
        delivery = {
            "id": "d1", "request": {"method": "POST", "path": "/events", "encoding": "json"},
            "mapping": [{"source": "event.type", "target": "kind", "location": "body"}],
        }
        preview = adapter.preview("", self.EVENT, delivery)
        self.assertEqual(preview["url"], "http://server.example/base/events")
        self.assertEqual(preview["body"]["kind"], "demo")

    def test_dify_preview_uses_body_and_file_inputs(self):
        adapter = DifyWorkflowAdapter({"api_url": "http://dify.example", "api_key": "secret"})
        delivery = {"mapping": [
            {"source": "event.type", "target": "prompt", "location": "body"},
            {
                "source": "media.annotated_image", "target": "image", "location": "file",
                "transform": "file", "file_mode": "list",
            },
        ]}
        preview = adapter.preview("", self.EVENT, delivery)
        self.assertEqual(preview["inputs"]["prompt"], "demo")
        self.assertEqual(preview["inputs"]["image"]["file"], "image.jpg")


if __name__ == "__main__":
    unittest.main()
