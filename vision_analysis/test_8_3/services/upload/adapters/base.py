from dataclasses import dataclass
from typing import Any, Dict


@dataclass
class DeliveryResult:
    ok: bool
    detail: str
    status_code: int = 0
    response: Any = None


class DeliveryAdapter:
    adapter_id = ""

    def __init__(self, profile: Dict[str, Any]):
        self.profile = profile

    def preview(self, event_dir: str, event: Dict[str, Any], delivery: Dict[str, Any]) -> Dict[str, Any]:
        raise NotImplementedError

    def send(self, event_dir: str, event: Dict[str, Any], delivery: Dict[str, Any]) -> DeliveryResult:
        raise NotImplementedError
