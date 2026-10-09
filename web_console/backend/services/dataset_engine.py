"""Dataset socket transport; independent of HTTP, persistence and business modules."""
from __future__ import annotations

import json
import socket
from pathlib import Path


class DatasetEngineUnavailable(RuntimeError):
    """The engine is offline or its response is incomplete/invalid."""


class DatasetEngineRejected(RuntimeError):
    """The engine rejected a validly transported command."""


class DatasetEngineClient:
    def request(self, directory: Path, command: dict) -> tuple[dict, bytes]:
        paths = [directory / 'run.dataset.sock', directory / 'run.control.sock.dataset']
        path = next((path for path in paths if path.exists()), paths[0])
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(4)
                client.connect(str(path))
                client.sendall((json.dumps(command, ensure_ascii=False, allow_nan=False) + '\n').encode())
                with client.makefile('rb') as stream:
                    line = stream.readline(128 * 1024)
                    if not line.endswith(b'\n'):
                        raise ValueError('incomplete header')
                    response = json.loads(line)
                    if not isinstance(response, dict):
                        raise ValueError('invalid header')
                    size = response.get('bytes', 0)
                    if not isinstance(size, int) or not 0 <= size <= 12 * 1024**2:
                        raise ValueError('invalid image size')
                    image = stream.read(size)
                    if len(image) != size:
                        raise ValueError('incomplete JPEG')
        except (OSError, ValueError) as exc:
            raise DatasetEngineUnavailable(str(exc)) from exc
        if not response.get('ok'):
            raise DatasetEngineRejected(response.get('message', '采集操作未完成'))
        return response, image
