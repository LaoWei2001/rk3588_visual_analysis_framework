import asyncio
import json
import socket
import threading

from routers import file_playback


def _app(root):
    app_dir = root / "demo"
    app_dir.mkdir()
    return app_dir


def _serve_once(path, response, received, ready):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(str(path))
        server.listen(1)
        ready.set()
        connection, _ = server.accept()
        with connection:
            received.update(json.loads(connection.recv(64 * 1024).decode("utf-8")))
            connection.sendall((json.dumps(response) + "\n").encode("utf-8"))


def test_status_lists_local_file_sources(tmp_path, monkeypatch):
    app_dir = _app(tmp_path)
    monkeypatch.setattr(file_playback, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(file_playback.pm, "get_status", lambda _: {"status": "running"})
    socket_path = app_dir / "run.playback.sock"
    received = {}
    ready = threading.Event()
    thread = threading.Thread(target=_serve_once, args=(
        socket_path,
        {"ok": True, "sources": [{"owner_channel_id": 1, "channel_ids": [1, 3], "duration_ms": 5000}]},
        received,
        ready,
    ))
    thread.start()
    assert ready.wait(timeout=2)

    result = asyncio.run(file_playback.get_file_playback("demo"))
    thread.join(timeout=2)

    assert received == {"command": "status"}
    assert result["socket_ready"] is True
    assert result["sources"][0]["channel_ids"] == [1, 3]


def test_seek_forwards_channel_and_position(tmp_path, monkeypatch):
    app_dir = _app(tmp_path)
    monkeypatch.setattr(file_playback, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(file_playback.pm, "get_status", lambda _: {"status": "running"})
    socket_path = app_dir / "run.playback.sock"
    received = {}
    ready = threading.Event()
    thread = threading.Thread(target=_serve_once, args=(
        socket_path,
        {"ok": True, "channel_id": 3, "position_ms": 2450},
        received,
        ready,
    ))
    thread.start()
    assert ready.wait(timeout=2)

    result = asyncio.run(file_playback.seek_file_playback("demo", 3, file_playback.SeekRequest(position_ms=2500)))
    thread.join(timeout=2)

    assert received == {"command": "seek", "channel_id": 3, "position_ms": 2500}
    assert result["position_ms"] == 2450


def test_stopped_app_has_no_playback_controls(tmp_path, monkeypatch):
    _app(tmp_path)
    monkeypatch.setattr(file_playback, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(file_playback.pm, "get_status", lambda _: {"status": "stopped"})

    result = asyncio.run(file_playback.get_file_playback("demo"))

    assert result == {"socket_ready": False, "sources": []}


def test_status_fills_missing_duration_from_probe(tmp_path, monkeypatch):
    app_dir = _app(tmp_path)
    video = app_dir / "assets" / "odd.mp4"
    video.parent.mkdir()
    video.write_bytes(b"demo")
    monkeypatch.setattr(file_playback, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(file_playback.pm, "get_status", lambda _: {"status": "running"})
    monkeypatch.setattr(file_playback, "_probe_duration_ms", lambda *_: 123456)
    socket_path = app_dir / "run.playback.sock"
    received = {}
    ready = threading.Event()
    thread = threading.Thread(target=_serve_once, args=(
        socket_path,
        {"ok": True, "sources": [{
            "owner_channel_id": 0,
            "channel_ids": [0],
            "location": "assets/odd.mp4",
            "duration_ms": 0,
            "seekable": True,
        }]},
        received,
        ready,
    ))
    thread.start()
    assert ready.wait(timeout=2)

    result = asyncio.run(file_playback.get_file_playback("demo"))
    thread.join(timeout=2)

    assert result["sources"][0]["duration_ms"] == 123456
