import asyncio
import json
import socket
import threading

from routers import logic_control


def _write_demo_app(root):
    app_dir = root / "demo"
    (app_dir / "assets").mkdir(parents=True)
    config = {
        "global": {
            "global_logics": [
                {
                    "instance_id": "aggregate-1",
                    "enable": True,
                    "logic": "global_channel_aggregate_demo",
                }
            ]
        },
        "channels": [{"id": 7, "enable": True, "logic": "logic_course_07"}],
    }
    catalog = {
        "channel_logics": [
            {"name": "logic_course_07", "label": "通道按钮", "actions": [{"id": "change"}]}
        ],
        "global_logics": [
            {
                "name": "global_channel_aggregate_demo",
                "label": "全局聚合",
                "actions": [{"id": "reset_report"}],
            }
        ],
    }
    (app_dir / "assets" / "config.json").write_text(json.dumps(config), encoding="utf-8")
    (app_dir / "logics.json").write_text(json.dumps(catalog), encoding="utf-8")
    return app_dir


def test_lists_channel_and_global_actions(tmp_path, monkeypatch):
    _write_demo_app(tmp_path)
    monkeypatch.setattr(logic_control, "APPS_ROOT", tmp_path)

    result = asyncio.run(logic_control.get_logic_actions("demo"))

    assert result["channels"][0]["channel_id"] == 7
    assert result["channels"][0]["actions"][0]["id"] == "change"
    assert result["globals"][0]["instance_id"] == "aggregate-1"
    assert result["globals"][0]["actions"][0]["id"] == "reset_report"


def test_global_action_uses_global_scope_and_instance(tmp_path, monkeypatch):
    app_dir = _write_demo_app(tmp_path)
    monkeypatch.setattr(logic_control, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(logic_control.pm, "get_status", lambda _: {"status": "running"})

    socket_path = app_dir / "run.control.sock"
    received = {}
    ready = threading.Event()

    def serve():
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
            server.bind(str(socket_path))
            server.listen(1)
            ready.set()
            connection, _ = server.accept()
            with connection:
                received.update(json.loads(connection.recv(64 * 1024).decode("utf-8")))
                connection.sendall(b'{"ok":true,"message":"accepted"}\n')

    thread = threading.Thread(target=serve)
    thread.start()
    assert ready.wait(timeout=2)

    result = asyncio.run(
        logic_control.post_global_action(
            "demo",
            "aggregate-1",
            "reset_report",
            logic_control.LogicActionRequest(payload={"force": True}),
        )
    )
    thread.join(timeout=2)

    assert result["ok"] is True
    assert received["scope"] == "global"
    assert received["instance_id"] == "aggregate-1"
    assert received["action"] == "reset_report"
    assert received["payload"] == {"force": True}
