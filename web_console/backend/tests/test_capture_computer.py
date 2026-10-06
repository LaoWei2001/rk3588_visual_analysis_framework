import io
import json
import shutil
import socket
import subprocess
import threading
import time
from pathlib import Path

import pytest
from services import video_capture_manager as capture


def test_computer_pipeline_has_separate_video_and_preview_pipes():
    for codec in ('h264', 'h265'):
        source = {'source_type': 'rtsp', 'rtsp_url': 'rtsp://127.0.0.1/test', 'usb_device': ''}
        probe = {'codec': codec, 'width': 1280, 'height': 720, 'fps': 25}
        args = capture.build_record_args(source, probe, 17)
        assert 'filesink' not in args
        assert 'fd=17' in args and 'fd=1' in args
        assert 'streamable=true' in args
        assert args.count('mp4mux') == 1


@pytest.fixture
def real_manager(monkeypatch):
    if not shutil.which(capture.GSTREAMER) or not shutil.which(capture.FFPROBE):
        pytest.skip('GStreamer/ffprobe unavailable')
    source = {'source_type': 'usb', 'usb_device': '/dev/video0', 'rtsp_url': ''}
    probe = {'codec': 'h264', 'input_format': 'I420', 'width': 320, 'height': 240, 'fps': 10}
    monkeypatch.setattr(capture, 'probe_source', lambda _: (source, probe))
    monkeypatch.setattr(capture, '_usb_ingest', lambda *_: [
        'videotestsrc', 'is-live=true', 'pattern=snow', '!',
        'video/x-raw,width=320,height=240,framerate=10/1', '!',
    ])
    manager = capture.VideoCaptureManager()
    yield manager
    manager.shutdown()


def test_recording_streams_playable_mp4_without_creating_board_file(real_manager):
    result = real_manager.start_recording({}, 64 * 1024**2)
    assert result['destination'] == 'computer' and result['storage'] is None
    assert real_manager._working_path is None and real_manager._save_directory is None
    filename = result['output_path']
    assert '/' not in filename and not Path(filename).exists()
    content = io.BytesIO()
    stream = real_manager.open_recording_stream(result['download_id'])
    def consume():
        for chunk in stream:
            content.write(chunk)
    reader = threading.Thread(target=consume)
    reader.start()
    try:
        deadline = time.monotonic() + 8
        while content.tell() < 50000:
            assert time.monotonic() < deadline
            time.sleep(0.1)
        final = real_manager.stop_recording()
        reader.join(timeout=6)
        assert not reader.is_alive()
        assert final['state'] == 'completed', final
        assert not Path(filename).exists()
        probe = subprocess.run([capture.FFPROBE, '-v', 'error', '-i', 'pipe:0',
                                '-show_entries', 'stream=codec_name,width,height', '-of', 'json'],
                               input=content.getvalue(), capture_output=True, timeout=10)
        assert probe.returncode == 0, probe.stderr
        video = json.loads(probe.stdout)['streams'][0]
        assert video == {'codec_name': 'h264', 'width': 320, 'height': 240}
    finally:
        if reader.is_alive():
            real_manager.shutdown()
            reader.join(timeout=6)


def test_cancelled_computer_download_stops_capture(real_manager):
    result = real_manager.start_recording({}, 64 * 1024**2)
    stream = real_manager.open_recording_stream(result['download_id'])
    while not next(stream):
        pass
    stream.close()
    assert real_manager._record_done.wait(timeout=6)
    status = real_manager.status()
    assert not status['recording'] and not status['process_alive']
    assert status['stop_reason'] == 'download_disconnected'


def test_file_size_limit_stops_and_completes_stream(real_manager):
    result = real_manager.start_recording({}, 64 * 1024**2)
    with real_manager._lock:
        real_manager._max_file_size_bytes = 200000
    content = b''.join(real_manager.open_recording_stream(result['download_id']))
    assert real_manager._record_done.wait(timeout=6)
    final = real_manager.status()
    assert final['state'] == 'completed', final
    assert final['stop_reason'] == 'size_limit'
    assert b'ftyp' in content and b'moof' in content


def test_http_download_disconnect_releases_recording_process(real_manager):
    from urllib.request import build_opener, ProxyHandler
    import uvicorn
    from fastapi import FastAPI
    from fastapi.responses import StreamingResponse
    app = FastAPI()

    @app.get('/download/{download_id}')
    async def download(download_id: str):
        return StreamingResponse(real_manager.open_recording_stream(download_id), media_type='video/mp4')

    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    port = listener.getsockname()[1]
    server = uvicorn.Server(uvicorn.Config(app, log_level='error'))
    thread = threading.Thread(target=lambda: server.run(sockets=[listener]), daemon=True)
    thread.start()
    try:
        deadline = time.monotonic() + 5
        while not server.started:
            assert time.monotonic() < deadline
            time.sleep(0.02)
        result = real_manager.start_recording({}, 64 * 1024**2)
        with build_opener(ProxyHandler({})).open(f'http://127.0.0.1:{port}/download/{result["download_id"]}', timeout=5) as response:
            assert response.status == 200
            assert response.read(4096)
        assert real_manager._record_done.wait(timeout=6), 'HTTP cancellation left encoder running'
        assert real_manager.status()['stop_reason'] == 'download_disconnected'
    finally:
        server.should_exit = True
        thread.join(timeout=5)
        listener.close()
