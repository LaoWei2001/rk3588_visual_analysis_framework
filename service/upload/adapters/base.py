from dataclasses import dataclass
from typing import Any, Dict


@dataclass
class DeliveryResult:
    ok: bool
    detail: str
    status_code: int = 0
    response: Any = None
    terminal: bool = False


def is_retryable_http_status(status_code: int) -> bool:
    return status_code in (408, 425, 429) or status_code >= 500


class DeliveryAdapter:
    adapter_id = ""

    def __init__(self, connection: Dict[str, Any]):
        self.connection = connection

    def preview(self, event_dir: str, event: Dict[str, Any], delivery: Dict[str, Any]) -> Dict[str, Any]:
        raise NotImplementedError

    def send(self, event_dir: str, event: Dict[str, Any], delivery: Dict[str, Any]) -> DeliveryResult:
        raise NotImplementedError
