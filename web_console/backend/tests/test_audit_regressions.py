import asyncio
import io
import json
import socket
import threading
import time

import pytest
from fastapi import HTTPException, UploadFile

from routers import assets, config_io
from services.logic_client import send_action


def test_config_read_rejects_sibling_and_symlink(tmp_path, monkeypatch):
    root = tmp_path / "demo"
    (root / "assets").mkdir(parents=True)
    sibling = tmp_path / "demo_backup"
    sibling.mkdir()
    (sibling / "secret.json").write_text('{"secret": true}')
    (root / "assets" / "escape.json").symlink_to(sibling / "secret.json")
    monkeypatch.setattr(config_io, "APPS_ROOT", tmp_path)
    for path in ("../demo_backup/secret.json", "assets/escape.json"):
        with pytest.raises(HTTPException) as error:
            asyncio.run(config_io.load_config_file("demo", path))
        assert error.value.status_code == 400
    (root / "assets" / "valid.json").write_text('{"ok": true}')
    assert asyncio.run(config_io.load_config_file("demo", "assets/valid.json")) == {"ok": True}


def test_config_rejects_assets_directory_escape(tmp_path, monkeypatch):
    (tmp_path / "demo").mkdir()
    external = tmp_path / "outside"
    external.mkdir()
    (tmp_path / "demo" / "assets").symlink_to(external, target_is_directory=True)
    monkeypatch.setattr(config_io, "APPS_ROOT", tmp_path)
    with pytest.raises(HTTPException):
        asyncio.run(config_io.get_config("demo"))


def test_parallel_uploads_preserve_every_payload(tmp_path, monkeypatch):
    directory = tmp_path / "demo" / "assets"
    directory.mkdir(parents=True)
    monkeypatch.setattr(assets, "APPS_ROOT", tmp_path)
    (directory / "sample.txt").write_bytes(b"original")
    barrier = threading.Barrier(8)

    class ConcurrentFile(io.BytesIO):
        def seek(self, *args):
            barrier.wait(timeout=5)
            return super().seek(*args)

    async def upload_all():
        return await asyncio.gather(*[
            assets.upload_asset("demo", UploadFile(filename="sample.txt", file=ConcurrentFile(str(i).encode())), "false")
            for i in range(8)
        ])

    results = asyncio.run(upload_all())
    assert len({r["path"] for r in results}) == 8
    assert (directory / "sample.txt").read_bytes() == b"original"
    assert {p.read_bytes() for p in directory.iterdir()} == {b"original", *(str(i).encode() for i in range(8))}
    assert not list(directory.glob(".upload_*"))


def test_failed_upload_keeps_existing_file(tmp_path, monkeypatch):
    directory = tmp_path / "demo" / "assets"
    directory.mkdir(parents=True)
    (directory / "sample.txt").write_bytes(b"original")
    monkeypatch.setattr(assets, "APPS_ROOT", tmp_path)

    def fail(src, dst):
        dst.write(b"partial")
        raise OSError("disk full")

    monkeypatch.setattr(assets.shutil, "copyfileobj", fail)
    with pytest.raises(HTTPException):
        asyncio.run(assets.upload_asset("demo", UploadFile(filename="sample.txt", file=io.BytesIO(b"new")), "true"))
    assert (directory / "sample.txt").read_bytes() == b"original"
    assert len(list(directory.iterdir())) == 1


def test_control_response_can_arrive_in_fragments(tmp_path):
    path = tmp_path / "control.sock"
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(str(path))
        server.listen(1)

        def serve():
            connection, _ = server.accept()
            with connection:
                connection.recv(4096)
                connection.sendall(b'{"ok":')
                time.sleep(.03)
                connection.sendall(b'true}\n')

        worker = threading.Thread(target=serve)
        worker.start()
        try:
            assert send_action(path, {"action": "test"}) == {"ok": True}
        finally:
            worker.join(timeout=3)
        assert not worker.is_alive()


def test_action_token_compatibility_and_session_fallback(monkeypatch):
    import main
    from starlette.requests import Request
    from starlette.responses import Response

    async def accepted(_request):
        return Response(status_code=204)

    def request(headers=()):
        return Request({"type": "http", "method": "POST", "path": "/api/apps/demo/channels/0/actions/toggle",
                        "query_string": b"", "headers": list(headers)})

    monkeypatch.delenv("LOGIC_ACTION_TOKEN", raising=False)
    assert asyncio.run(main.auth_middleware(request(), accepted)).status_code == 204
    monkeypatch.setenv("LOGIC_ACTION_TOKEN", "test-secret")
    assert asyncio.run(main.auth_middleware(request(), accepted)).status_code == 401
    assert asyncio.run(main.auth_middleware(request([(b"x-logic-token", b"wrong")]), accepted)).status_code == 401
    assert asyncio.run(main.auth_middleware(request([(b"x-logic-token", b"test-secret")]), accepted)).status_code == 204
    monkeypatch.setattr(main, "get_session", lambda token: {"username": "tester"} if token == "session" else None)
    assert asyncio.run(main.auth_middleware(request([(b"authorization", b"Bearer session")]), accepted)).status_code == 204
