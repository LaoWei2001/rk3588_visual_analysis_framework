#!/usr/bin/env python3
"""Application-scoped visual event delivery service."""

import os
import signal
import threading
from pathlib import Path

from event_outbox import EventOutboxForwarder


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    app_dir = script_dir.parent.parent
    data_dir = Path(os.environ.get("UPLOAD_DATA_DIR", str(app_dir / ".runtime")))
    store_dir = Path(os.environ.get("EVENT_STORE_DIR", str(data_dir / "event_store")))
    data_dir.mkdir(parents=True, exist_ok=True)
    store_dir.mkdir(parents=True, exist_ok=True)

    shutdown = threading.Event()

    def stop(signum, _frame):
        print(f"[UploadService] received signal {signum}")
        shutdown.set()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    forwarder = EventOutboxForwarder(
        str(store_dir),
        data_dir / "connections.yaml",
        app_dir / "report_templates",
        data_dir / "report_contracts",
        data_dir / "contract_revisions",
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
