import asyncio

from fastapi import HTTPException

from routers import assets
from services import app_scanner


def test_list_assets_only_returns_real_files(tmp_path, monkeypatch):
    monkeypatch.setattr(assets, "APPS_ROOT", tmp_path)
    assets_dir = tmp_path / "demo" / "assets"
    assets_dir.mkdir(parents=True)

    (assets_dir / "detector.rknn").write_bytes(b"model")
    (assets_dir / "LABELS.TXT").write_text("person\n", encoding="utf-8")
    (assets_dir / "sample.MP4").write_bytes(b"video")
    (assets_dir / "fake.rknn").mkdir()
    (assets_dir / "config.json").write_text("{}", encoding="utf-8")

    result = asyncio.run(assets.list_assets("demo"))

    assert result == {
        "models": ["assets/detector.rknn"],
        "labels": ["assets/LABELS.TXT"],
        "videos": ["assets/sample.MP4"],
    }


def test_list_assets_rejects_unknown_app(tmp_path, monkeypatch):
    monkeypatch.setattr(assets, "APPS_ROOT", tmp_path)

    try:
        asyncio.run(assets.list_assets("missing"))
    except HTTPException as exc:
        assert exc.status_code == 404
    else:
        raise AssertionError("missing app must return HTTP 404")


def test_app_scanner_counts_only_real_asset_files(tmp_path, monkeypatch):
    app_dir = tmp_path / "demo"
    assets_dir = app_dir / "assets"
    assets_dir.mkdir(parents=True)
    (app_dir / "vision_analysis").write_bytes(b"binary")
    (assets_dir / "model.RKNN").write_bytes(b"model")
    (assets_dir / "labels.TXT").write_text("person\n", encoding="utf-8")
    (assets_dir / "clip.MKV").write_bytes(b"video")
    (assets_dir / "fake.rknn").mkdir()
    (assets_dir / "config.JSON").write_text("{}", encoding="utf-8")

    monkeypatch.setattr(app_scanner, "APPS_ROOT", tmp_path)
    monkeypatch.setattr(app_scanner, "get_status", lambda _name: {
        "status": "stopped", "mode": None, "pid": None, "uptime_seconds": None,
    })
    monkeypatch.setattr(app_scanner, "get_vision_settings", lambda _name: {
        "autostart": False, "desired_running": False,
    })
    monkeypatch.setattr(app_scanner, "data_dir", lambda name: tmp_path / ".data" / name)

    [result] = app_scanner.scan_apps()

    assert result["models"] == ["assets/model.RKNN"]
    assert result["labels"] == ["assets/labels.TXT"]
    assert result["videos"] == ["assets/clip.MKV"]
    assert result["config_files"] == ["assets/config.JSON"]
