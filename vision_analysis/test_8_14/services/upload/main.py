#!/usr/bin/env python3
"""通用视觉事件上传服务入口。"""

import os
import signal
import threading
from pathlib import Path

import yaml

from contracts import load_contracts
from event_outbox import EventOutboxForwarder


def main() -> None:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    data_dir = os.environ.get("UPLOAD_DATA_DIR")
    if data_dir:
        config_path = os.path.join(data_dir, "upload_config.yaml")
        contracts_dir = Path(data_dir) / "contracts"
    else:
        config_path = os.path.join(script_dir, "config.yaml")
        contracts_dir = None

    if not os.path.isfile(config_path):
        raise FileNotFoundError(f"upload config not found: {config_path}")

    with open(config_path, "r", encoding="utf-8") as stream:
        config = yaml.safe_load(stream) or {}

    store_dir = os.environ.get("EVENT_STORE_DIR") or os.path.abspath(
        os.path.join(script_dir, "..", "..", "event_store")
    )
    os.makedirs(store_dir, exist_ok=True)
    contracts_dir.mkdir(parents=True, exist_ok=True) if contracts_dir else None

    shutdown = threading.Event()

    def stop(signum, _frame):
        print(f"[UploadService] received signal {signum}")
        shutdown.set()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    forwarder = EventOutboxForwarder(
        config, store_dir,
        contracts=load_contracts(contracts_dir) if contracts_dir else None,
    )
    worker = threading.Thread(
        target=forwarder.run,
        args=(shutdown,),
        name="EventOutboxForwarder",
        daemon=True,
    )
    worker.start()
    while worker.is_alive():
        worker.join(timeout=1)


if __name__ == "__main__":
    main()
