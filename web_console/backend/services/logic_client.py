"""Bounded, newline-framed communication with the engine control socket."""
import json
import socket
import time
from pathlib import Path

from fastapi import HTTPException


def send_action(socket_path: Path, message: dict) -> dict:
    deadline = time.monotonic() + 3.0
    data = bytearray()
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(3.0)
            client.connect(str(socket_path))
            client.sendall((json.dumps(message, ensure_ascii=False) + "\n").encode("utf-8"))
            while b"\n" not in data:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("control response timed out")
                client.settimeout(remaining)
                chunk = client.recv(min(4096, 65537 - len(data)))
                if not chunk:
                    break
                data.extend(chunk)
                if len(data) > 65536:
                    raise HTTPException(502, "control response too large")
    except OSError as exc:
        raise HTTPException(503, f"logic control unavailable: {exc}") from exc
    try:
        response = json.loads(bytes(data).split(b"\n", 1)[0].decode("utf-8"))
        if not isinstance(response, dict):
            raise ValueError("response must be an object")
    except (ValueError, UnicodeError) as exc:
        raise HTTPException(502, "invalid response from logic control") from exc
    if not response.get("ok"):
        raise HTTPException(409, response.get("message") or "logic action rejected")
    return response
