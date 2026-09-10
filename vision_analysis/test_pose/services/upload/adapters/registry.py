import json
from pathlib import Path
from typing import Any, Dict

from .base import DeliveryAdapter
from .dify_workflow import DifyWorkflowAdapter
from .http import HttpAdapter

_ADAPTERS = {
    HttpAdapter.adapter_id: HttpAdapter,
    DifyWorkflowAdapter.adapter_id: DifyWorkflowAdapter,
}


def create_adapter(adapter_id: str, connection: Dict[str, Any]) -> DeliveryAdapter:
    adapter_class = _ADAPTERS.get(adapter_id)
    if adapter_class is None:
        raise ValueError(f"unknown delivery adapter: {adapter_id}")
    return adapter_class(connection)


def adapter_catalog():
    path = Path(__file__).with_name("catalog.json")
    return json.loads(path.read_text(encoding="utf-8"))
