import json
import asyncio
import logging
import struct
from urllib.parse import urlencode

import pytest
from starlette.requests import Request

from routers import stream


def test_build_ffmpeg_args_uses_zero_transcode_fragmented_mp4():
    args = stream._build_ffmpeg_args("rtsp://127.0.0.1:9554/preview")

    assert args[0] == "ffmpeg"
    assert "gst-launch-1.0" not in args
    assert args[args.index("-i") + 1] == "rtsp://127.0.0.1:9554/preview"
    assert args[args.index("-c:v") + 1] == "copy"
    assert args[args.index("-f") + 1] == "mp4"
    assert args[args.index("-movflags") + 1] == "empty_moov+default_base_moof+frag_keyframe"
    assert args[args.index("-frag_duration") + 1] == "200000"
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


@pytest.mark.parametrize('cause', ['playback-stall', 'buffer-timeout', 'initialization-timeout', '', 'unrecognized-event'])
def test_client_retry_logs_only_known_events_and_playback_state(tmp_path, monkeypatch, caplog, cause):
    (tmp_path / 'demo').mkdir()
    monkeypatch.setattr(stream, 'APPS_ROOT', tmp_path)
    monkeypatch.setattr(stream.shutil, 'which', lambda _name: '/usr/bin/ffmpeg')
    query = urlencode({'retry_cause': cause, 'retry_position_ms': '12500',
                       'retry_ahead_ms': '300', 'retry_ready_state': '2',
                       'retry_paused': 'false', 'retry_seeking': 'false', 'token': 'do-not-log'})
    request = Request({'type': 'http', 'query_string': query.encode(), 'headers': []})
    with caplog.at_level(logging.WARNING, logger='uvicorn.error'):
        response = asyncio.run(stream.stream_app('demo', request))
    assert response.media_type == 'video/mp4'
    if cause in {'playback-stall', 'buffer-timeout', 'initialization-timeout'}:
        assert f'client retry: cause={cause}' in caplog.text
        assert "'position_ms': '12500'" in caplog.text
        assert "'ahead_ms': '300'" in caplog.text
    else:
        assert 'client retry:' not in caplog.text
    assert 'do-not-log' not in caplog.text


class StreamRequest:
    def __init__(self, client_id=None):
        self.query_params = {'client_id': client_id} if client_id else {}

    async def is_disconnected(self):
        return False


def box(kind, data=b''):
    return struct.pack('>I4s', 8 + len(data), kind) + data


INIT = box(b'ftyp', b'isom') + box(b'moov')
FRAGMENT = (box(b'moof', box(b'traf',
    box(b'tfhd', struct.pack('>III', 0x20, 1, 0x02000000)) +
    box(b'trun', struct.pack('>III', 4, 1, 0x02000000)))) + box(b'mdat', b'frame'))


class MuxProcess:
    pid = 123

    def __init__(self):
        self.stdout = asyncio.StreamReader()
        self.stdout.feed_data(INIT + FRAGMENT)
        self.stderr = None
        self.returncode = None

    def terminate(self):
        self.returncode = 0
        self.stdout.feed_eof()

    async def wait(self):
        return self.returncode


@pytest.fixture
def mux(monkeypatch):
    processes = []

    async def create(*_args, **_kwargs):
        process = MuxProcess()
        processes.append(process)
        return process

    monkeypatch.setattr(stream, '_active_streams', {})
    monkeypatch.setattr(stream, '_replace_locks', {})
    monkeypatch.setattr(stream, '_stream_pool', stream.StreamPool())
    monkeypatch.setattr(stream.asyncio, 'create_subprocess_exec', create)
    return processes


@pytest.mark.parametrize('identified', [True, False])
def test_two_viewers_share_source_without_disconnecting_each_other(mux, identified):
    async def exercise():
        first = stream._fmp4_stream(StreamRequest('first' if identified else None), 'demo', 'rtsp://local/live')
        second = stream._fmp4_stream(StreamRequest('second' if identified else None), 'demo', 'rtsp://local/live')
        try:
            assert await first.__anext__() == INIT
            assert await first.__anext__() == FRAGMENT
            assert await second.__anext__() == INIT
            assert await second.__anext__() == FRAGMENT
            assert len(mux) == 1
            assert len(stream._active_streams) == 2
            await second.aclose()
            assert mux[0].returncode is None
            mux[0].stdout.feed_data(FRAGMENT)
            assert await first.__anext__() == FRAGMENT
        finally:
            await first.aclose()
            await second.aclose()
            await stream._stream_pool.shutdown()
        assert not stream._active_streams
        assert mux[0].returncode == 0
        assert not stream._stream_pool.sources

    asyncio.run(exercise())


def test_reconnect_replaces_only_same_viewer_without_restarting_source(mux):
    async def exercise():
        first = stream._fmp4_stream(StreamRequest('viewer'), 'demo', 'rtsp://local/live')
        other = stream._fmp4_stream(StreamRequest('other'), 'demo', 'rtsp://local/live')
        replacement = stream._fmp4_stream(StreamRequest('viewer'), 'demo', 'rtsp://local/live')
        try:
            assert await first.__anext__() == INIT
            assert await other.__anext__() == INIT
            assert await replacement.__anext__() == INIT
            assert len(mux) == 1
            with pytest.raises(StopAsyncIteration):
                await first.__anext__()
            assert len(stream._active_streams) == 2
            assert await other.__anext__() == FRAGMENT
            assert await replacement.__anext__() == FRAGMENT
            assert mux[0].returncode is None
        finally:
            await first.aclose()
            await other.aclose()
            await replacement.aclose()
            await stream._stream_pool.shutdown()
        assert not stream._active_streams

    asyncio.run(exercise())


def test_cancelled_disconnect_releases_only_its_subscription(mux):
    async def exercise():
        generator = stream._fmp4_stream(StreamRequest('viewer'), 'demo', 'rtsp://local/live')
        try:
            await generator.__anext__()
            await generator.__anext__()
            session = stream._active_streams[('demo', 'viewer')]
            pending = asyncio.create_task(generator.__anext__())
            await asyncio.sleep(0)
            pending.cancel()
            with pytest.raises(asyncio.CancelledError):
                await pending
            assert session.done.is_set()
            assert not stream._active_streams
            assert not session.source.viewers
            assert mux[0].returncode is None  # Grace period keeps the encoder warm.
        finally:
            await generator.aclose()
            await stream._stream_pool.shutdown()
        assert mux[0].returncode == 0

    asyncio.run(exercise())


def test_replacement_does_not_wait_for_blocked_old_http_sender(monkeypatch):
    monkeypatch.setattr(stream, '_active_streams', {})
    monkeypatch.setattr(stream, '_replace_locks', {})

    async def exercise():
        old = stream._StreamSession(asyncio.Event(), asyncio.Event())
        stream._active_streams[('demo', 'first')] = old
        replacement = stream._StreamSession(asyncio.Event(), asyncio.Event())
        newcomer = stream._StreamSession(asyncio.Event(), asyncio.Event())
        await asyncio.wait_for(stream._replace_session('demo', 'first', replacement), timeout=.1)
        await asyncio.wait_for(stream._replace_session('demo', 'second', newcomer), timeout=.1)
        assert old.stop.is_set()
        assert not old.done.is_set()
        assert stream._active_streams[('demo', 'first')] is replacement
        assert stream._active_streams[('demo', 'second')] is newcomer
        assert not stream._replace_locks

    asyncio.run(exercise())


def test_cancelled_waiter_preserves_lock_for_other_waiters(monkeypatch):
    monkeypatch.setattr(stream, '_replace_locks', {})

    async def exercise():
        key = ('demo', 'first')
        acquired = []

        async def waiter(name):
            async with stream._replacement_lock(key):
                acquired.append(name)

        async with stream._replacement_lock(key):
            entry = stream._replace_locks[key]
            cancelled = asyncio.create_task(waiter('cancelled'))
            survivor = asyncio.create_task(waiter('survivor'))
            await asyncio.sleep(0)
            assert entry.users == 3
            cancelled.cancel()
            with pytest.raises(asyncio.CancelledError):
                await cancelled
            assert entry.users == 2
            assert stream._replace_locks[key] is entry
            assert acquired == []
        await survivor
        assert acquired == ['survivor']
        assert not stream._replace_locks

    asyncio.run(exercise())
