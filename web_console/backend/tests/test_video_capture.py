from datetime import datetime, timezone
from pathlib import Path

import pytest

from services import video_capture_manager as capture


def test_storage_snapshot_uses_selected_directory_filesystem(tmp_path, monkeypatch):
    target = tmp_path / "recordings"
    target.mkdir()
    monkeypatch.setattr(capture, "ALLOWED_ROOTS", (tmp_path.resolve(),))
    monkeypatch.setattr(capture, "MIN_FREE_RESERVE_BYTES", 1024)
    monkeypatch.setattr(capture, "FREE_RESERVE_RATIO", 0.0)
    monkeypatch.setattr(capture, "FINALIZE_MARGIN_BYTES", 128)
    monkeypatch.setattr(capture, "_mount_details", lambda _path: (str(tmp_path), "ext4"))

    result = capture.storage_snapshot(str(target))

    assert result["path"] == str(target.resolve())
    assert result["mount_point"] == str(tmp_path)
    assert result["filesystem"] == "ext4"
    assert result["available_bytes"] > 0
    assert result["safe_available_bytes"] == result["available_bytes"] - 1024
    assert result["max_recording_file_bytes"] == result["safe_available_bytes"] - 128


def test_storage_path_must_exist_and_stay_inside_allowed_roots(tmp_path, monkeypatch):
    allowed = tmp_path / "allowed"
    outside = tmp_path / "outside"
    allowed.mkdir()
    outside.mkdir()
    monkeypatch.setattr(capture, "ALLOWED_ROOTS", (allowed.resolve(),))

    with pytest.raises(capture.VideoCaptureInputError, match="绝对路径"):
        capture.resolve_capture_directory("relative/path")
    with pytest.raises(capture.VideoCaptureInputError, match="允许的目录"):
        capture.resolve_capture_directory(str(outside))
    with pytest.raises(capture.VideoCaptureInputError, match="不存在"):
        capture.resolve_capture_directory(str(allowed / "missing"))


def test_fat32_single_file_limit_is_reported(tmp_path, monkeypatch):
    target = tmp_path / "recordings"
    target.mkdir()
    monkeypatch.setattr(capture, "ALLOWED_ROOTS", (tmp_path.resolve(),))
    monkeypatch.setattr(capture, "MIN_FREE_RESERVE_BYTES", 0)
    monkeypatch.setattr(capture, "FREE_RESERVE_RATIO", 0.0)
    monkeypatch.setattr(capture, "FINALIZE_MARGIN_BYTES", 0)
    monkeypatch.setattr(capture, "_mount_details", lambda _path: (str(tmp_path), "vfat"))

    result = capture.storage_snapshot(str(target))

    assert result["filesystem_max_file_bytes"] == 4 * 1024**3 - 1
    assert result["max_recording_file_bytes"] <= 4 * 1024**3 - 1
    with pytest.raises(capture.VideoCaptureInputError, match="ext4"):
        capture.validate_recording_capacity(result, 4 * 1024**3)


def test_capacity_check_rejects_request_larger_than_safe_space():
    snapshot = {
        "writable": True,
        "filesystem_max_file_bytes": None,
        "max_recording_file_bytes": 100,
    }
    with pytest.raises(capture.VideoCaptureInputError, match="安全可用空间"):
        capture.validate_recording_capacity(snapshot, 101)
    capture.validate_recording_capacity(snapshot, 100)


def test_rtsp_record_pipeline_is_original_stream_to_single_mp4(tmp_path):
    url = "rtsp://admin:secret@192.168.10.2/Streaming/Channels/101"
    source = {"source_type": "rtsp", "rtsp_url": url, "usb_device": ""}
    probe = {"codec": "h264", "width": 1920, "height": 1080, "fps": 25}
    output = tmp_path / ".20260826_143025_123456.mp4.part"

    args = capture.build_record_args(source, probe, output)
    rendered = " ".join(args)

    assert args[0:3] == [capture.GSTREAMER, "-q", "-e"]
    assert f"location={url}" in args
    assert "rtph264depay" in args
    assert "mppvideodec" in args  # 仅供低帧率 Web 预览
    assert "mpph264enc" not in args  # RTSP 录像分支不重新编码
    assert rendered.count("mp4mux") == 1
    assert rendered.count("filesink") == 1
    assert f"location={output}" in args
    assert "fragment-duration=1000" in args


def test_h265_rtsp_record_pipeline_uses_mpp_h264_normalization(tmp_path):
    url = "rtsp://admin:secret@192.168.10.3/Streaming/Channels/101"
    source = {"source_type": "rtsp", "rtsp_url": url, "usb_device": ""}
    probe = {"codec": "h265", "width": 3840, "height": 2160, "fps": 25}
    output = tmp_path / ".h265-camera.mp4.part"

    args = capture.build_record_args(source, probe, output)
    rendered = " ".join(args)

    assert "rtph265depay" in args
    assert "mppvideodec" in args
    assert "format=NV12" in args
    assert "name=capture_decoded" in args
    assert "mpph264enc" in args
    assert "rc-mode=vbr" in args
    assert "profile=main" in args
    assert "level=5.1" in args
    assert "header-mode=each-idr" in args
    assert "video/x-h264,stream-format=avc,alignment=au" in args
    assert "video/x-h265,stream-format=hvc1,alignment=au" not in args
    assert rendered.count("mp4mux") == 1
    assert rendered.count("filesink") == 1
    assert f"location={output}" in args


def test_pipeline_error_summary_prefers_root_error_over_rtsp_pause_noise():
    lines = [
        "ERROR: from element /GstPipeline:pipeline0/GstMP4Mux:mp4mux0: Could not multiplex stream.",
        "Additional debug info:",
        "gstqtmux.c: Buffer has no PTS.",
        "ERROR: from element /GstPipeline:pipeline0/GstRTSPSrc:rtspsrc0: Could not write to resource.",
        "Additional debug info:",
        "gstrtspsrc.c: gst_rtspsrc_pause (): Could not send message. (Received end-of-file)",
    ]

    result = capture._pipeline_error_summary(lines, "fallback")

    assert "Could not multiplex stream" in result
    assert "Buffer has no PTS" in result
    assert "gst_rtspsrc_pause" not in result


def test_pipeline_error_summary_keeps_rtsp_error_when_it_is_the_only_error():
    lines = [
        "错误：来自组件 /GstPipeline:pipeline0/GstRTSPSrc:rtspsrc0：无法写入资源。",
        "额外的调试信息：",
        "gstrtspsrc.c: gst_rtspsrc_pause (): Could not send message. (Received end-of-file)",
    ]

    result = capture._pipeline_error_summary(lines, "fallback")

    assert "gst_rtspsrc_pause" in result


def test_usb_mjpeg_record_pipeline_uses_color_safe_x264(tmp_path):
    source = {
        "source_type": "usb", "rtsp_url": "", "usb_device": "/dev/video3",
        "usb_width": 1280, "usb_height": 720,
    }
    probe = {
        "codec": "h264", "input_format": "MJPG",
        "width": 1280, "height": 720, "fps": 30,
    }
    args = capture.build_record_args(source, probe, tmp_path / ".capture.mp4.part")
    rendered = " ".join(args)

    assert "v4l2src" in args
    assert "device=/dev/video3" in args
    assert "image/jpeg,width=1280,height=720,framerate=30/1" in args
    assert "jpegparse" in args
    assert "jpegdec" in args
    assert "mppjpegdec" not in args
    assert "video/x-raw,format=I420" in args
    assert "name=capture_raw" in args
    assert "x264enc" in args
    assert "pass=cbr" in args
    assert "speed-preset=superfast" in args
    assert "qp-min=18" in args
    assert "qp-max=32" in args
    assert "avmux_mp4" not in args
    assert "mpph264enc" not in args
    assert rendered.count("mp4mux") == 1
    assert rendered.count("filesink") == 1
    assert not any("yolo" in value.lower() or "logic" in value.lower() for value in args)


def test_usb_raw_fallback_avoids_mpp_alignment_bug(tmp_path):
    source = {
        "source_type": "usb", "rtsp_url": "", "usb_device": "/dev/video3",
        "usb_width": 640, "usb_height": 360,
    }
    probe = {
        "codec": "h264", "input_format": "NV12",
        "width": 640, "height": 360, "fps": 30,
    }

    args = capture.build_record_args(source, probe, tmp_path / ".capture.mp4.part")

    assert "x264enc" in args
    assert "mpph264enc" not in args
    assert "video/x-raw,format=I420" in args
    assert "video/x-raw,format=NV12,width=640,height=360,framerate=30/1" in args


def test_h264_encoder_rates_balance_quality_and_file_size():
    target_720p, _, maximum_720p = capture._h264_encoder_rates({
        "width": 1280, "height": 720, "fps": 30,
    })
    target_1080p, _, maximum_1080p = capture._h264_encoder_rates({
        "width": 1920, "height": 1080, "fps": 30,
    })
    target_4k, _, maximum_4k = capture._h264_encoder_rates({
        "width": 3840, "height": 2160, "fps": 30,
    })

    assert target_720p == 4_147_200
    assert target_1080p == 9_331_200
    assert target_4k == 20_000_000
    assert maximum_720p == 5_184_000
    assert maximum_1080p == 11_664_000
    assert maximum_4k == 25_000_000


def test_usb_modes_are_parsed_and_best_format_is_selected():
    output = """
        [0]: 'YUYV' (YUYV 4:2:2)
            Size: Discrete 1920x1080
                Interval: Discrete 0.200s (5.000 fps)
            Size: Discrete 640x480
                Interval: Discrete 0.033s (30.000 fps)
        [1]: 'MJPG' (Motion-JPEG, compressed)
            Size: Discrete 1920x1080
                Interval: Discrete 0.033s (30.000 fps)
                Interval: Discrete 0.040s (25.000 fps)
            Size: Discrete 640x480
                Interval: Discrete 0.033s (30.000 fps)
        [2]: 'NV12' (Y/CbCr 4:2:0)
            Size: Discrete 640x480
                Interval: Discrete 0.033s (30.000 fps)
    """

    modes = capture._parse_v4l2_modes(output)
    options = capture._usb_resolution_options(modes)

    assert options == [
        {"width": 1920, "height": 1080, "max_fps": 30.0},
        {"width": 640, "height": 480, "max_fps": 30.0},
    ]
    assert capture._select_usb_mode(modes, 1920, 1080)["pixel_format"] == "MJPG"
    assert capture._select_usb_mode(modes, 640, 480)["pixel_format"] == "MJPG"


def test_usb_resolution_is_part_of_normalized_source():
    result = capture._normalize_source({
        "source_type": "usb",
        "usb_device": "/dev/video81",
        "usb_width": 1920,
        "usb_height": 1080,
    })

    assert result["usb_width"] == 1920
    assert result["usb_height"] == 1080
    with pytest.raises(capture.VideoCaptureInputError, match="同时设置"):
        capture._normalize_source({
            "source_type": "usb", "usb_device": "/dev/video81", "usb_width": 1280,
        })


def test_filename_is_local_recording_start_time():
    value = datetime(2026, 8, 26, 14, 30, 25, 327000, tzinfo=timezone.utc)
    assert capture._timestamp_stem(value) == "20260826_143025_327000"


def test_preview_stream_wraps_latest_complete_jpeg():
    class RunningProcess:
        def poll(self):
            return None

    manager = capture.VideoCaptureManager()
    frame = b"\xff\xd8jpeg-payload\xff\xd9"
    with manager._lock:
        manager._process = RunningProcess()
        manager._state = "previewing"
        manager._generation = 1
        manager._latest_preview_frame = frame
        manager._preview_sequence = 1

    stream = manager.open_preview_stream()
    chunk = next(iter(stream))

    assert chunk.startswith(b"--frame\r\nContent-Type: image/jpeg")
    assert f"Content-Length: {len(frame)}\r\n".encode() in chunk
    assert chunk.endswith(frame + b"\r\n")
    stream.close()
