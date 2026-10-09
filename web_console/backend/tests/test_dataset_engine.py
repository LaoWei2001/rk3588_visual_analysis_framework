"""Exercise the transport boundary with real sockets and malformed peer responses."""
from concurrent.futures import ThreadPoolExecutor
import json
import socket

import pytest

from services.dataset_engine import DatasetEngineClient, DatasetEngineRejected, DatasetEngineUnavailable


@pytest.mark.parametrize('chunks, expected, error', [
    ([b'{"ok":true,"tasks":[]}\n'], ({'ok': True, 'tasks': []}, b''), None),
    ([b'{"ok":true,"bytes":6}\n', b'abc', b'def'], ({'ok': True, 'bytes': 6}, b'abcdef'), None),
    ([b'not-json\n'], None, DatasetEngineUnavailable),
    ([b'[]\n'], None, DatasetEngineUnavailable),
    ([b'{"ok":true,"bytes":12582913}\n'], None, DatasetEngineUnavailable),
    ([b'{"ok":true,"bytes":6}\nabc'], None, DatasetEngineUnavailable),
    ([b'{"ok":false,"message":"paused"}\n'], None, DatasetEngineRejected),
], ids=['status', 'jpeg', 'json', 'object', 'limit', 'truncated', 'rejected'])
def test_wire(tmp_path, chunks, expected, error):
    path = tmp_path / 'run.dataset.sock'
    command = {'op': 'status'}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(str(path))
        listener.listen(1)
        listener.settimeout(2)

        def serve():
            connection, _ = listener.accept()
            with connection, connection.makefile('rb') as stream:
                connection.settimeout(2)
                assert json.loads(stream.readline()) == command
                for chunk in chunks:
                    connection.sendall(chunk)

        with ThreadPoolExecutor(max_workers=1) as pool:
            peer = pool.submit(serve)
            client = DatasetEngineClient()
            if error:
                with pytest.raises(error):
                    client.request(tmp_path, command)
            else:
                assert client.request(tmp_path, command) == expected
            peer.result(timeout=3)
