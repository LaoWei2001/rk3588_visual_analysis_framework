import asyncio
import io
import json
import zipfile

import pytest
from fastapi import HTTPException
from routers import records


def event(root, event_id, raw=None, annotated=None):
    path = root / event_id
    path.mkdir()
    (path / "event.json").write_text(json.dumps({"event": {"id": event_id}}))
    if raw is not None:
        (path / "raw.jpg").write_bytes(raw)
    if annotated is not None:
        (path / "annotated.jpg").write_bytes(annotated)
    return path


def test_selected_export_preserves_original_bytes_and_does_not_substitute_annotations(tmp_path, monkeypatch):
    monkeypatch.setenv("EVENT_STORE_DIR", str(tmp_path))
    raw = b"original-jpeg" * 20000
    event(tmp_path, "one", raw=raw, annotated=b"annotation")
    event(tmp_path, "two", annotated=b"annotation-only")
    event(tmp_path, "three", raw=b"unselected")
    result = asyncio.run(records.prepare_image_export("demo", records.RecordSelection(ids=["one", "two", "one"])))
    assert result["image_count"] == 1
    assert result["skipped_count"] == 1
    chunks = list(records._stream_raw_images("demo", result["images"]))
    assert max(map(len, chunks)) <= 65536
    with zipfile.ZipFile(io.BytesIO(b"".join(chunks))) as archive:
        assert archive.namelist() == ["one_raw.jpg"]
        assert archive.read("one_raw.jpg") == raw
    with pytest.raises(HTTPException, match="404"):
        asyncio.run(records.record_original_image("demo", "two"))


def test_export_all_includes_images_beyond_display_limit_and_handles_deleted_image(tmp_path, monkeypatch):
    monkeypatch.setenv("EVENT_STORE_DIR", str(tmp_path))
    for i in range(505):
        event(tmp_path, f"event-{i}", raw=f"raw-{i}".encode())
    result = asyncio.run(records.prepare_image_export("demo", records.RecordSelection()))
    assert result["image_count"] == 505
    (tmp_path / result["images"][0]["id"] / "raw.jpg").unlink()
    content = b"".join(records._stream_raw_images("demo", result["images"]))
    with zipfile.ZipFile(io.BytesIO(content)) as archive:
        assert len([name for name in archive.namelist() if name.endswith('.jpg')]) == 504
        assert 'missing-images.txt' in archive.namelist()
        assert archive.testzip() is None


def test_delete_selected_only_removes_selected_records_and_rejects_invalid_scope(tmp_path, monkeypatch):
    monkeypatch.setenv("EVENT_STORE_DIR", str(tmp_path))
    event(tmp_path, "selected", raw=b"raw")
    untouched = event(tmp_path, "untouched", raw=b"raw")
    result = asyncio.run(records.delete_selected_records("demo", records.RecordSelection(ids=["selected", "selected", "gone"])))
    assert result["deleted_ids"] == ["selected"]
    assert untouched.is_dir()
    for invalid in (None, [], [".."], ["selected", "../untouched"]):
        with pytest.raises(HTTPException):
            asyncio.run(records.delete_selected_records("demo", records.RecordSelection(ids=invalid)))
    assert untouched.is_dir()


def test_export_download_is_scoped_to_app_and_expires(tmp_path, monkeypatch):
    monkeypatch.setenv("EVENT_STORE_DIR", str(tmp_path))
    event(tmp_path, "one", raw=b"raw")
    job = asyncio.run(records.prepare_image_export("first", records.RecordSelection()))
    with pytest.raises(HTTPException):
        asyncio.run(records.download_image_export("other", job['download_id']))
    monkeypatch.setattr(records.time, "monotonic", lambda: float('inf'))
    with pytest.raises(HTTPException):
        asyncio.run(records.download_image_export("first", job['download_id']))
