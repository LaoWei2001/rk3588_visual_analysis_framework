#!/usr/bin/env python3
"""统一告警事件上传服务入口。"""

import os
import signal
import threading

import yaml

from dify_uploader import DifyUploader
from event_outbox import EventOutboxForwarder


def main() -> None:
    script_dir = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(script_dir, "config.yaml"), "r", encoding="utf-8") as stream:
        config = yaml.safe_load(stream) or {}

    store_dir = os.environ.get("ALARM_STORE_DIR") or os.path.abspath(
        os.path.join(script_dir, "..", "..", "alarm_store")
    )
    os.makedirs(store_dir, exist_ok=True)

    shutdown = threading.Event()

    def stop(signum, _frame):
        print(f"[UploadService] received signal {signum}")
        shutdown.set()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    forwarder = EventOutboxForwarder(config, store_dir, DifyUploader(config))
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
