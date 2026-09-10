import json

from routers import stream


def test_build_ffmpeg_args_uses_zero_transcode_fragmented_mp4():
    args = stream._build_ffmpeg_args("rtsp://127.0.0.1:9554/preview")

    assert args[0] == "ffmpeg"
    assert "gst-launch-1.0" not in args
    assert args[args.index("-i") + 1] == "rtsp://127.0.0.1:9554/preview"
    assert args[args.index("-c:v") + 1] == "copy"
    assert args[args.index("-f") + 1] == "mp4"
    assert args[args.index("-movflags") + 1] == "frag_keyframe+empty_moov+default_base_moof"
    assert args[-1] == "pipe:1"


def test_rtsp_info_reads_active_runtime_config(tmp_path, monkeypatch):
    app_dir = tmp_path / "demo"
    assets_dir = app_dir / "assets"
    assets_dir.mkdir(parents=True)
    (app_dir / "run.config").write_text("preview.json\n", encoding="utf-8")
    (assets_dir / "preview.json").write_text(
        json.dumps(
            {
                "global": {
                    "rtsp_port": 9554,
                    "rtsp_path": "preview",
                    "rtsp_codec": "H264",
                }
            }
        ),
        encoding="utf-8",
    )
    monkeypatch.setattr(stream, "APPS_ROOT", tmp_path)

    assert stream._rtsp_info("demo") == ("rtsp://127.0.0.1:9554/preview", "h264")
