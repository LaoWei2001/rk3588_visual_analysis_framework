import asyncio
import struct
import subprocess
from types import SimpleNamespace

import pytest

from services import live_stream as live


@pytest.fixture(scope="module")
def video(tmp_path_factory):
    path = tmp_path_factory.mktemp('shared-live') / 'sample.mp4'
    subprocess.run([
        'ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
        'testsrc2=size=320x180:rate=10', '-t', '6', '-an', '-c:v', 'libx264',
        '-preset', 'ultrafast', '-tune', 'zerolatency', '-g', '10', '-sc_threshold', '0',
        '-pix_fmt', 'yuv420p', '-movflags', 'empty_moov+default_base_moof+frag_keyframe',
        '-frag_duration', '200000', str(path),
    ], check=True, timeout=30)
    return path.read_bytes()


def test_arbitrary_pipe_boundaries_and_late_join_are_decodable(video, tmp_path):
    parser = live.FragmentParser()
    fragments = []
    for offset in range(0, len(video), 71):
        fragments.extend(parser.feed(video[offset:offset + 71]))
    init = fragments[0]
    assert init[1]
    keys = [index for index, (_, initial, keyframe) in enumerate(fragments) if keyframe]
    assert len(keys) == 6
    # Every join point must decode independently, preserving the source timeline.
    for number, index in enumerate(keys):
        end = keys[number + 1] if number + 1 < len(keys) else len(fragments)
        path = tmp_path / f'join-{number}.mp4'
        path.write_bytes(init[0] + b''.join(data for data, _, _ in fragments[index:end]))
        result = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(path), '-f', 'null', '-'],
                                capture_output=True, timeout=10)
        assert result.returncode == 0 and not result.stderr, result.stderr.decode()


def test_slow_viewer_never_blocks_fast_viewer_and_memory_is_bounded(video):
    async def exercise():
        source = live.SharedSource('demo', [], lambda _source: None)
        slow, fast = source.subscribe(), source.subscribe()
        for fragment in live.FragmentParser().feed(video):
            source.publish(*fragment)
            assert await fast.read() == fragment[0]
        # A deliberately stalled HTTP response cannot accumulate unlimited data.
        data = b'x' * (live.MAX_QUEUE_BYTES // 2)
        source.publish(data, False, True)
        assert await fast.read() == data
        source.publish(data, False, False)
        assert await fast.read() == data
        source.publish(data, False, False)
        assert await fast.read() == data
        assert slow.reason == 'slow-consumer'
        assert slow.pending_bytes == 0
        assert await slow.read() is None
        assert slow not in source.viewers and fast in source.viewers
        assert fast.pending_bytes == 0
        source.stop()
    asyncio.run(exercise())


def test_viewer_queue_age_limit(monkeypatch):
    clock = [0.0]
    monkeypatch.setattr(live, 'time', SimpleNamespace(monotonic=lambda: clock[0]))
    async def exercise():
        viewer = live.Viewer()
        viewer.push(b'old')
        clock[0] = live.MAX_QUEUE_AGE + .01
        viewer.push(b'new')
        assert viewer.reason == 'slow-consumer'
        assert viewer.pending_bytes == 0
    asyncio.run(exercise())


def test_late_viewer_gets_init_latest_keyframe_and_continuation(video):
    async def exercise():
        source = live.SharedSource('demo', [], lambda _source: None)
        parts = list(live.FragmentParser().feed(video))
        for part in parts[:18]:
            source.publish(*part)
        last_key = max(i for i, part in enumerate(parts[:18]) if part[2])
        late = source.subscribe()
        assert await late.read() == parts[0][0]
        for data, _, _ in parts[last_key:18]:
            assert await late.read() == data
        source.publish(*parts[18])
        assert await late.read() == parts[18][0]
        source.stop()
    asyncio.run(exercise())


def test_oversized_gop_waits_for_next_keyframe_instead_of_serving_partial_data(video, monkeypatch):
    async def exercise():
        source = live.SharedSource('demo', [], lambda _source: None)
        parts = list(live.FragmentParser().feed(video))
        source.publish(*parts[0])
        source.publish(*parts[1])
        source.publish(b'x' * live.MAX_QUEUE_BYTES, False, False)
        late = source.subscribe()
        assert await late.read() == parts[0][0]
        assert late.waiting_for_keyframe
        source.publish(*parts[2])
        assert not late.queue
        next_key = next(part for part in parts[2:] if part[2])
        source.publish(*next_key)
        assert await late.read() == next_key[0]
        source.stop()
    asyncio.run(exercise())


def test_pool_grace_period_shares_single_process_and_stops_it(video, monkeypatch):
    processes = []

    class Process:
        pid = 123
        stderr = None
        returncode = None

        def __init__(self):
            self.stdout = asyncio.StreamReader()
            self.stdout.feed_data(video)

        def terminate(self):
            self.returncode = 0
            self.stdout.feed_eof()

        async def wait(self):
            return self.returncode

    async def create(*_args, **_kwargs):
        process = Process()
        processes.append(process)
        return process

    monkeypatch.setattr(live.asyncio, 'create_subprocess_exec', create)
    monkeypatch.setattr(live, 'IDLE_GRACE_SECONDS', .03)

    async def exercise():
        pool = live.StreamPool()
        source, first = pool.subscribe('demo', 'rtsp://local/live', [])
        assert await first.read()
        source.unsubscribe(first)
        successor, second = pool.subscribe('demo', 'rtsp://local/live', [])
        assert successor is source
        assert await second.read()
        assert len(processes) == 1 and processes[0].returncode is None
        source.unsubscribe(second)
        await asyncio.wait_for(source.task, timeout=.5)
        assert processes[0].returncode == 0
        assert not pool.sources
        # A new source cannot be removed by an old source's late finalizer.
        newest, last = pool.subscribe('demo', 'rtsp://local/live', [])
        source.on_end(source)
        assert pool.sources[('demo', 'rtsp://local/live')] is newest
        assert await last.read()
        await pool.shutdown()
        assert all(process.returncode == 0 for process in processes)
    asyncio.run(exercise())


@pytest.mark.parametrize('size', [0, 7, live.MAX_BOX_BYTES + 1])
def test_parser_rejects_invalid_and_unbounded_boxes(size):
    with pytest.raises(ValueError):
        list(live.FragmentParser().feed(struct.pack('>I4s', size, b'mdat')))


def test_truncated_fragment_stops_source_and_releases_all_viewers(monkeypatch):
    class Process:
        pid = 123
        stderr = None
        returncode = None

        def __init__(self):
            self.stdout = asyncio.StreamReader()
            self.stdout.feed_data(struct.pack('>I4s', 0, b'moov'))

        def terminate(self):
            self.returncode = 0

        async def wait(self):
            return self.returncode

    async def create(*_args, **_kwargs):
        return Process()
    monkeypatch.setattr(live.asyncio, 'create_subprocess_exec', create)

    async def exercise():
        pool = live.StreamPool()
        source, viewer = pool.subscribe('demo', 'rtsp://local/live', [])
        assert await viewer.read() is None
        assert viewer.reason == 'upstream-error'
        await source.task
        assert not pool.sources
        assert source.process.returncode == 0
    asyncio.run(exercise())
