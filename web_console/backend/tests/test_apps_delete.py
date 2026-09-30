import asyncio

from routers import apps


def _prepare_app(tmp_path, name: str = "demo"):
    app_dir = tmp_path / name
    app_dir.mkdir()
    (app_dir / "vision_analysis").write_text("binary", encoding="utf-8")
    data_dir = tmp_path / ".data" / name
    event_dir = data_dir / "event_store" / "event-1"
    event_dir.mkdir(parents=True)
    (event_dir / "annotated.jpg").write_bytes(b"image")
    return app_dir, data_dir


def _isolate(tmp_path, monkeypatch):
    monkeypatch.setattr(apps, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(apps.pm, "stop_app", lambda _name: None)
    monkeypatch.setattr(apps.runtime_state, "remove_vision_app", lambda _name: None)


def test_delete_app_keeps_runtime_data_by_default(tmp_path, monkeypatch):
    _isolate(tmp_path, monkeypatch)
    app_dir, data_dir = _prepare_app(tmp_path)

    result = asyncio.run(apps.delete_app("demo"))

    assert result == {"ok": True, "data_deleted": False}
    assert not app_dir.exists()
    assert (data_dir / "event_store" / "event-1" / "annotated.jpg").is_file()


def test_delete_app_can_remove_package_and_all_runtime_data(tmp_path, monkeypatch):
    _isolate(tmp_path, monkeypatch)
    app_dir, data_dir = _prepare_app(tmp_path)

    result = asyncio.run(apps.delete_app("demo", delete_data=True))

    assert result == {"ok": True, "data_deleted": True}
    assert not app_dir.exists()
    assert not data_dir.exists()
