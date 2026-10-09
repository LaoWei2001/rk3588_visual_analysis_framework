import asyncio
import json
import socket
import threading
from concurrent.futures import ThreadPoolExecutor

import pytest
from fastapi import HTTPException

from routers import config_io
from routers import assets as assets_router
from services import model_info


def detector(size=640):
    outputs = []
    for stride in (8, 16, 32):
        for channels in (64, 2, 1):
            outputs.append({"format": "NCHW", "shape": [1, channels, size // stride, size // stride]})
    return {"ok": True, "width": size, "height": size, "format": "NHWC", "outputs": outputs}


@pytest.fixture
def app(tmp_path, monkeypatch):
    directory = tmp_path / "demo"
    assets = directory / "assets"
    assets.mkdir(parents=True)
    (directory / "vision_analysis").write_bytes(b"binary")
    (assets / "small.rknn").write_bytes(b"small")
    (assets / "large.rknn").write_bytes(b"large")
    model_info._cache.clear()
    monkeypatch.setattr(config_io, "APPS_ROOT", tmp_path)
    return directory


def config(*paths):
    return {"channels": [{"id": 2, "models": [
        {"id": f"det_{i}", "model_type": "yolov8_det", "model_path": f"assets/{path}.rknn"}
        for i, path in enumerate(paths)]}]}


def test_probe_cache_single_flight_and_file_replacement(app, monkeypatch):
    calls = []
    def probe(*args):
        calls.append(args)
        return detector()
    monkeypatch.setattr(model_info, "_probe", probe)
    with ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(lambda _: model_info.inspect_model(app, "assets/small.rknn", "yolov8_det"), range(16)))
    assert len(calls) == 1
    assert all(result["ok"] and result["width"] == 640 for result in results)
    # A mismatched decoder must not poison the file's cached tensor metadata.
    assert not model_info.inspect_model(app, "assets/small.rknn", "yolov5")["ok"]
    assert model_info.inspect_model(app, "assets/small.rknn", "yolov8_det")["ok"]
    results[0]["outputs"].clear()
    assert len(model_info.inspect_model(app, "assets/small.rknn")["outputs"]) == 9
    replacement = app / "assets" / "replacement"
    replacement.write_bytes(b"replacement model")
    replacement.replace(app / "assets" / "small.rknn")
    fresh = model_info.inspect_model(app, "assets/small.rknn")
    assert len(calls) == 2
    assert fresh["file_version"] != results[0]["file_version"]
    (app / "vision_analysis").write_bytes(b"new binary")
    model_info.inspect_model(app, "assets/small.rknn")
    assert len(calls) == 3


def test_cached_probe_failure_expires(app, monkeypatch):
    clock = [0.0]
    calls = []
    monkeypatch.setattr(model_info.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(model_info, "_probe", lambda *args: calls.append(args) or {"ok": False, "error": "bad model"})
    for _ in range(2):
        assert not model_info.inspect_model(app, "assets/small.rknn")["ok"]
    assert len(calls) == 1
    clock[0] = 31.0
    model_info.inspect_model(app, "assets/small.rknn")
    assert len(calls) == 2


@pytest.mark.parametrize("path", ["../small.rknn", "/etc/model.rknn", "assets/missing.rknn", "assets/config.json"])
def test_invalid_paths_do_not_launch_probe(app, monkeypatch, path):
    monkeypatch.setattr(model_info, "_probe", lambda *args: pytest.fail("probe should not run"))
    with pytest.raises(ValueError):
        model_info.inspect_model(app, path)


def test_symlink_outside_assets_is_rejected(app, tmp_path):
    outside = tmp_path / "outside.rknn"
    outside.write_bytes(b"model")
    (app / "assets" / "escape.rknn").symlink_to(outside)
    with pytest.raises(ValueError):
        model_info.inspect_model(app, "assets/escape.rknn")


@pytest.mark.parametrize("target", ["default", "named"])
def test_mixed_input_size_rejected_before_writing(app, monkeypatch, target):
    monkeypatch.setattr(model_info, "_probe", lambda _, __, path: detector(960 if path.stem == "large" else 640))
    destination = app / "assets" / ("config.json" if target == "default" else "custom.json")
    destination.write_text('{"old":true}')
    with pytest.raises(HTTPException) as error:
        if target == "default":
            asyncio.run(config_io.save_config("demo", config("small", "large")))
        else:
            asyncio.run(config_io.save_config_file("demo", "assets/custom.json", config("small", "large")))
    assert error.value.status_code == 422
    assert "640×640" in error.value.detail and "960×960" in error.value.detail
    assert json.loads(destination.read_text()) == {"old": True}


def test_disabled_models_and_empty_draft_need_no_probe(app, monkeypatch):
    monkeypatch.setattr(model_info, "_probe", lambda *args: pytest.fail("probe should not run"))
    model_info.validate_model_config(app, {})
    disabled = config("missing")
    disabled["channels"][0]["models"][0]["enable"] = False
    model_info.validate_model_config(app, disabled)
    disabled["channels"][0]["models"][0]["enable"] = True
    disabled["channels"][0]["infer_enable"] = False
    model_info.validate_model_config(app, disabled)


def test_valid_960_can_be_saved(app, monkeypatch):
    monkeypatch.setattr(model_info, "_probe", lambda *args: detector(960))
    assert asyncio.run(config_io.save_config("demo", config("large"))) == {"ok": True}
    assert json.loads((app / "assets" / "config.json").read_text()) == config("large")


def test_decoder_input_layout_and_output_contract():
    info = detector()
    info["format"] = "NCHW"
    assert not model_info.compatibility_error(info, "yolov8_det")
    assert "NHWC" in model_info.compatibility_error(info, "yolov5")
    info["outputs"][0]["shape"][2] = 79
    assert model_info.compatibility_error(info, "yolov8_det")
    assert model_info.compatibility_error(detector(), "unknown")


def test_fused_pose_output_checks_quantization_and_element_count():
    output = {"shape": [1, 56, 300], "elements": 56 * 300, "type": 1, "scale": 0}
    info = {"format": "NHWC", "outputs": [output], "width": 960, "height": 960}
    assert not model_info.compatibility_error(info, "yolo26_pose")
    output.update(type=2, scale=2.0)
    assert "scale" in model_info.compatibility_error(info, "yolo26_pose")
    output.update(type=1, scale=0, elements=1)
    assert "元素" in model_info.compatibility_error(info, "yolo26_pose")


def test_segmentation_proto_size_guard():
    outputs = []
    for stride in (8, 16, 32):
        outputs.extend([
            {"format": "NCHW", "shape": [1, 21, 960 // stride, 960 // stride]},
            {"format": "NCHW", "shape": [1, 96, 960 // stride, 960 // stride]},
        ])
    outputs.append({"format": "NCHW", "shape": [1, 32, 240, 240]})
    info = {"width": 960, "height": 960, "format": "NHWC", "outputs": outputs}
    assert "proto" in model_info.compatibility_error(info, "yolov5_seg")
    outputs[-1]["shape"] = [1, 32, 160, 160]
    assert not model_info.compatibility_error(info, "yolov5_seg")


def test_missing_model_type_is_rejected_without_probe(app, monkeypatch):
    monkeypatch.setattr(model_info, "_probe", lambda *args: pytest.fail("probe should not run"))
    invalid = config("small")
    invalid["channels"][0]["models"][0].pop("model_type")
    with pytest.raises(ValueError, match="model_type"):
        model_info.validate_model_config(app, invalid)


def test_metadata_route_and_stopped_runtime_route(app, monkeypatch):
    monkeypatch.setattr(assets_router, "APPS_ROOT", app.parent)
    monkeypatch.setattr(model_info, "_probe", lambda *args: detector())
    monkeypatch.setattr(model_info.pm, "get_status", lambda _: {"status": "stopped", "config": None})
    result = asyncio.run(assets_router.get_model_info("demo", "assets/small.rknn", "yolov8_det"))
    assert result["ok"] and result["width"] == 640
    assert asyncio.run(assets_router.get_model_runtime("demo"))["status"] == "stopped"
    with pytest.raises(HTTPException) as error:
        asyncio.run(assets_router.get_model_info("../demo", "assets/small.rknn"))
    assert error.value.status_code == 422


def test_probe_protocol_and_timeout(app, monkeypatch):
    import subprocess
    binary, model = app / "vision_analysis", app / "assets" / "small.rknn"
    def execute(*args, **kwargs):
        assert args[0] == [str(binary), "--inspect-model", str(model)]
        assert kwargs["timeout"] == 20
        assert str(app / "libs") in kwargs["env"]["LD_LIBRARY_PATH"]
        return subprocess.CompletedProcess(args[0], 0, "vendor log\nMODEL_INFO_JSON=" + json.dumps(detector()) + "\n", "")
    monkeypatch.setattr(model_info.subprocess, "run", execute)
    assert model_info._probe(app, binary, model)["ok"]
    monkeypatch.setattr(model_info.subprocess, "run", lambda *args, **kwargs: subprocess.CompletedProcess([], 1, "old engine\n", ""))
    assert "更新" in model_info._probe(app, binary, model)["error"]
    def timeout(*args, **kwargs):
        raise subprocess.TimeoutExpired(args[0], 20)
    monkeypatch.setattr(model_info.subprocess, "run", timeout)
    assert "超时" in model_info._probe(app, binary, model)["error"]


def test_runtime_reads_actual_failed_reload_snapshot_in_chunks(app, monkeypatch):
    monkeypatch.setattr(model_info.pm, "get_status", lambda _: {"status": "running", "config": "live.json"})
    snapshot = {"ok": True, "business_width": 640, "business_height": 640, "channels": [
        {"channel_id": 2, "state": "failed", "error": "RKNN init failed", "active_models": [
            {"id": "det", "model_path": "assets/small.rknn", "width": 640, "height": 640}]}]}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(str(app / "run.control.sock"))
        server.listen(1)
        def serve():
            connection, _ = server.accept()
            with connection:
                request = json.loads(connection.recv(1024))
                assert request == {"scope": "models", "action": "status"}
                data = json.dumps(snapshot).encode() + b"\n"
                connection.sendall(data[:12])
                connection.sendall(data[12:])
        thread = threading.Thread(target=serve)
        thread.start()
        result = model_info.runtime_status(app, "demo")
        thread.join(timeout=2)
    assert result["config"] == "live.json"
    assert result["channels"][0]["active_models"][0]["width"] == 640
    assert result["channels"][0]["state"] == "failed"


def test_stopped_runtime_does_not_open_socket(app, monkeypatch):
    monkeypatch.setattr(model_info.pm, "get_status", lambda _: {"status": "stopped", "config": None})
    monkeypatch.setattr(model_info.socket, "socket", lambda *args: pytest.fail("socket should not open"))
    assert model_info.runtime_status(app, "demo") == {"status": "stopped", "config": None, "channels": []}
