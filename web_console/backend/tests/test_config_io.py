import errno
import json
import socket
import threading
import time
from urllib.error import HTTPError
from urllib.request import ProxyHandler, Request, build_opener

import pytest
from fastapi import FastAPI
import uvicorn

from routers import config_io


@pytest.fixture
def environment(tmp_path, monkeypatch):
    assets = tmp_path / "demo" / "assets"
    assets.mkdir(parents=True)
    monkeypatch.setattr(config_io, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(config_io, "validate_model_config", lambda *_: None)
    app = FastAPI()
    app.include_router(config_io.router)
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    url = f"http://127.0.0.1:{listener.getsockname()[1]}"
    server = uvicorn.Server(uvicorn.Config(app, log_level="error", lifespan="off"))
    worker = threading.Thread(target=lambda: server.run(sockets=[listener]), daemon=True)
    worker.start()
    opener = build_opener(ProxyHandler({}))

    def post(path, body):
        request = Request(url + path, data=json.dumps(body).encode(),
                          headers={"Content-Type": "application/json"})
        try:
            response = opener.open(request, timeout=5)
        except HTTPError as error:
            response = error
        with response:
            return response.status, json.loads(response.read())

    try:
        deadline = time.monotonic() + 3
        while not server.started and time.monotonic() < deadline:
            time.sleep(0.01)
        assert server.started
        yield post, assets
    finally:
        server.should_exit = True
        worker.join(timeout=5)
        listener.close()
        assert not worker.is_alive()


@pytest.mark.parametrize("filename", ["config.json", "custom.json"])
@pytest.mark.parametrize("failure_stage", ["create", "write", "replace"])
def test_disk_full_returns_clear_error_and_preserves_roi_config(
    environment, monkeypatch, filename, failure_stage,
):
    post, assets = environment
    target = assets / filename
    original = '{"channels": [{"id": 0, "roi_zones": []}]}'
    target.write_text(original)
    payload = {"channels": [{"id": 0, "roi_zones": [
        {"name": "采集区", "polygon": [[0.1, 0.1], [0.8, 0.1], [0.8, 0.8]]},
    ]}]}

    def fail(*_args, **_kwargs):
        raise OSError(errno.ENOSPC, "No space left on device")

    if failure_stage == "create":
        monkeypatch.setattr(config_io.tempfile, "NamedTemporaryFile", fail)
    elif failure_stage == "write":
        def partial_write(_data, handle, **_kwargs):
            handle.write('{"channels":')
            fail()
        monkeypatch.setattr(config_io.json, "dump", partial_write)
    else:
        monkeypatch.setattr(config_io.os, "replace", fail)
    url = "/apps/demo/config" if filename == "config.json" else "/apps/demo/config-file?path=assets/custom.json"
    status, response = post(url, payload)
    assert status == 507
    assert "空间不足" in response["detail"]
    assert target.read_text() == original
    assert list(assets.iterdir()) == [target]


def test_successful_save_replaces_config_and_cleans_temporary_file(environment):
    post, assets = environment
    target = assets / "config.json"
    target.write_text('{"old": true}')
    target.chmod(0o640)
    payload = {"channels": [{"id": 0, "roi_zones": [{"name": "采集区"}]}]}
    assert post("/apps/demo/config", payload)[0] == 200
    assert json.loads((assets / "config.json").read_text()) == payload
    assert target.stat().st_mode & 0o777 == 0o640
    assert [path.name for path in assets.iterdir()] == ["config.json"]


@pytest.mark.parametrize("code, phrase", [
    (errno.EDQUOT, "空间不足"), (errno.EACCES, "写入权限"), (errno.EROFS, "只读"),
])
def test_other_storage_errors_have_readable_detail(environment, monkeypatch, code, phrase):
    post, assets = environment
    target = assets / "config.json"
    target.write_text("{}")

    def fail(*_args):
        raise OSError(code, "storage error")

    monkeypatch.setattr(config_io.os, "replace", fail)
    status, response = post("/apps/demo/config", {"channels": []})
    assert status == (507 if code == errno.EDQUOT else 500)
    assert phrase in response["detail"]
    assert target.read_text() == "{}"
    assert list(assets.iterdir()) == [target]
