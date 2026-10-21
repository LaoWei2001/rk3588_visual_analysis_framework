"""One RTSP transmuxer per source, with independent bounded viewer queues.

Late viewers receive the initialization segment and the latest complete GOP.
Only immutable MP4 fragments are shared; a blocked HTTP sender never holds up
the producer or another viewer. All state belongs to the console's event loop.
"""

import asyncio
import logging
import struct
import time
from collections import deque
from contextlib import suppress
from typing import Callable, Optional

log = logging.getLogger("uvicorn.error")
MAX_BOX_BYTES = 16 * 1024 * 1024
MAX_INIT_BYTES = 1024 * 1024
MAX_QUEUE_BYTES = 4 * 1024 * 1024
MAX_QUEUE_FRAGMENTS = 32
MAX_QUEUE_AGE = 3.0
IDLE_GRACE_SECONDS = 3.0


def _boxes(data: bytes, offset: int = 0):
    while offset < len(data):
        if len(data) - offset < 8:
            raise ValueError("Truncated MP4 box")
        size, kind = struct.unpack_from(">I4s", data, offset)
        header = 8
        if size == 1:
            if len(data) - offset < 16:
                raise ValueError("Truncated extended MP4 box")
            size = struct.unpack_from(">Q", data, offset + 8)[0]
            header = 16
        if size < header or offset + size > len(data):
            raise ValueError("Invalid MP4 box size")
        yield kind, data[offset + header:offset + size]
        offset += size


def _starts_with_keyframe(moof: bytes) -> bool:
    """Read the first video sample flags from FFmpeg's single-track fragment."""
    body = next(_boxes(moof))[1]
    for kind, traf in _boxes(body):
        if kind != b"traf":
            continue
        default_flags = None
        for child, payload in _boxes(traf):
            if len(payload) < 8:
                raise ValueError("Truncated fragment header")
            flags = int.from_bytes(payload[1:4], "big")
            if child == b"tfhd":
                offset = 8 + (8 if flags & 1 else 0)
                offset += 4 * sum(bool(flags & bit) for bit in (2, 8, 16))
                if flags & 32:
                    default_flags = struct.unpack_from(">I", payload, offset)[0]
            elif child == b"trun" and struct.unpack_from(">I", payload, 4)[0]:
                offset = 8 + (4 if flags & 1 else 0)
                sample_flags = default_flags
                if flags & 4:
                    sample_flags = struct.unpack_from(">I", payload, offset)[0]
                elif flags & 0x400:
                    offset += 4 * sum(bool(flags & bit) for bit in (0x100, 0x200))
                    sample_flags = struct.unpack_from(">I", payload, offset)[0]
                return (sample_flags is not None and not sample_flags & 0x10000
                        and (sample_flags >> 24) & 3 != 1)
    return False


class FragmentParser:
    """Frame arbitrary pipe reads into init and complete moof/mdat pairs."""

    def __init__(self):
        self.pending = bytearray()
        self.init = bytearray()
        self.ready = False
        self.moof: Optional[bytes] = None

    def feed(self, data: bytes):
        self.pending.extend(data)
        while len(self.pending) >= 8:
            size = int.from_bytes(self.pending[:4], "big")
            kind = self.pending[4:8]
            header = 8
            if size == 1:
                if len(self.pending) < 16:
                    return
                size = int.from_bytes(self.pending[8:16], "big")
                header = 16
            if size < header or size > MAX_BOX_BYTES:
                raise ValueError("Invalid or oversized live MP4 box")
            if len(self.pending) < size:
                return
            box = bytes(self.pending[:size])
            del self.pending[:size]
            if not self.ready:
                self.init.extend(box)
                if len(self.init) > MAX_INIT_BYTES:
                    raise ValueError("Oversized live MP4 initialization")
                if kind == b"moov":
                    self.ready = True
                    yield bytes(self.init), True, False
            elif kind == b"moof":
                if self.moof is not None:
                    raise ValueError("Live MP4 fragment is missing mdat")
                self.moof = box
            elif kind == b"mdat":
                if self.moof is None:
                    raise ValueError("Live MP4 mdat is missing moof")
                keyframe = _starts_with_keyframe(self.moof)
                yield self.moof + box, False, keyframe
                self.moof = None


class Viewer:
    def __init__(self):
        self.queue = deque()
        self.ready = asyncio.Event()
        self.pending_bytes = 0
        self.reason = ""
        self.waiting_for_keyframe = True

    def push(self, data: bytes):
        if self.reason:
            return
        now = time.monotonic()
        # Queue age is independent of fragment sizes and source bitrate.
        oldest = self.queue[0][1] if self.queue else now
        if (len(self.queue) >= MAX_QUEUE_FRAGMENTS or self.pending_bytes + len(data) > MAX_QUEUE_BYTES
                or now - oldest > MAX_QUEUE_AGE):
            self.close("slow-consumer")
            return
        self.queue.append((data, now))
        self.pending_bytes += len(data)
        self.ready.set()

    def close(self, reason: str):
        if self.reason:
            return
        self.reason = reason
        self.queue.clear()
        self.pending_bytes = 0
        self.queue.append((None, time.monotonic()))
        self.ready.set()

    async def read(self) -> Optional[bytes]:
        await self.ready.wait()
        data, _ = self.queue.popleft()
        if not self.queue:
            self.ready.clear()
        if data is not None:
            self.pending_bytes -= len(data)
        return data


async def terminate_process(proc):
    if proc is None or proc.returncode is not None:
        return
    try:
        proc.terminate()
        try:
            await asyncio.wait_for(proc.wait(), timeout=2)
        except asyncio.TimeoutError:
            proc.kill()
            await proc.wait()
    except ProcessLookupError:
        pass


class SharedSource:
    def __init__(self, name: str, args: list[str], on_end: Callable):
        self.name = name
        self.args = args
        self.on_end = on_end
        self.viewers: set[Viewer] = set()
        self.init: Optional[bytes] = None
        self.gop: list[bytes] = []
        self.gop_bytes = 0
        self.gop_at = 0.0
        self.ended = False
        self.process = None
        self.task: Optional[asyncio.Task] = None
        self.idle_handle: Optional[asyncio.TimerHandle] = None

    def subscribe(self) -> Viewer:
        if self.idle_handle is not None:
            self.idle_handle.cancel()
            self.idle_handle = None
        viewer = Viewer()
        if self.init is not None:
            viewer.push(self.init)
            if self.gop and time.monotonic() - self.gop_at <= MAX_QUEUE_AGE:
                viewer.waiting_for_keyframe = False
                for fragment in self.gop:
                    viewer.push(fragment)
        self.viewers.add(viewer)
        return viewer

    def unsubscribe(self, viewer: Viewer):
        viewer.close("client-disconnected")
        self.viewers.discard(viewer)
        if not self.viewers and not self.ended and self.idle_handle is None:
            # A page refresh/reconnect should not tear down the hardware encoder.
            self.idle_handle = asyncio.get_running_loop().call_later(IDLE_GRACE_SECONDS, self.stop)

    def stop(self):
        self.ended = True
        if self.idle_handle is not None:
            self.idle_handle.cancel()
            self.idle_handle = None
        self.on_end(self)
        for viewer in self.viewers:
            viewer.close("source-stopped")
        self.viewers.clear()
        if self.task is not None:
            self.task.cancel()

    def publish(self, data: bytes, initial: bool, keyframe: bool):
        if initial:
            self.init = data
        else:
            if keyframe:
                self.gop = []
                self.gop_bytes = 0
                self.gop_at = time.monotonic()
            if keyframe or self.gop:
                self.gop.append(data)
                self.gop_bytes += len(data)
                if self.gop_bytes > MAX_QUEUE_BYTES - MAX_INIT_BYTES or len(self.gop) > MAX_QUEUE_FRAGMENTS - 2:
                    # Never serve a partial GOP to a late viewer.
                    self.gop = []
                    self.gop_bytes = 0
        for viewer in tuple(self.viewers):
            if not initial and viewer.waiting_for_keyframe:
                if not keyframe:
                    continue
                viewer.waiting_for_keyframe = False
            viewer.push(data)
            if viewer.reason:
                log.warning("[LiveStream][%s] viewer removed: reason=%s; other viewers continue",
                            self.name, viewer.reason)
                self.unsubscribe(viewer)

    async def _stderr(self):
        if self.process.stderr is None:
            return
        while True:
            line = await self.process.stderr.readline()
            if not line:
                break
            log.warning("[LiveStream][%s] shared ffmpeg: %s", self.name, line.decode(errors="replace").strip())

    async def run(self):
        reason = "source-stopped"
        stderr_task = None
        started = time.monotonic()
        fragments = total_bytes = 0
        try:
            self.process = await asyncio.create_subprocess_exec(
                *self.args, stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE)
            log.info("[LiveStream][%s] shared source started: pid=%s", self.name, self.process.pid)
            stderr_task = asyncio.create_task(self._stderr())
            parser = FragmentParser()
            last_fragment_at = time.monotonic()
            while True:
                chunk = await asyncio.wait_for(self.process.stdout.read(65536), timeout=15)
                if not chunk:
                    reason = "upstream-eof"
                    break
                for fragment, initial, keyframe in parser.feed(chunk):
                    self.publish(fragment, initial, keyframe)
                    last_fragment_at = time.monotonic()
                    fragments += not initial
                    total_bytes += len(fragment)
                if time.monotonic() - last_fragment_at > 15:
                    reason = "incomplete-fragment-timeout"
                    break
        except asyncio.TimeoutError:
            reason = "upstream-timeout"
        except asyncio.CancelledError:
            pass
        except Exception:
            reason = "upstream-error"
            log.exception("[LiveStream][%s] shared source failed", self.name)
        finally:
            self.ended = True
            if self.idle_handle is not None:
                self.idle_handle.cancel()
                self.idle_handle = None
            self.on_end(self)
            for viewer in self.viewers:
                viewer.close(reason)
            self.viewers.clear()
            await terminate_process(self.process)
            if stderr_task is not None:
                try:
                    await asyncio.wait_for(stderr_task, timeout=1)
                except asyncio.TimeoutError:
                    stderr_task.cancel()
                    with suppress(asyncio.CancelledError):
                        await stderr_task
            log.info("[LiveStream][%s] shared source closed: reason=%s seconds=%.1f fragments=%d bytes=%d",
                     self.name, reason, time.monotonic() - started, fragments, total_bytes)


class StreamPool:
    def __init__(self):
        self.sources: dict[tuple[str, str], SharedSource] = {}
        self.tasks: set[asyncio.Task] = set()

    def subscribe(self, name: str, url: str, args: list[str]) -> tuple[SharedSource, Viewer]:
        key = (name, url)
        source = self.sources.get(key)
        if source is None or source.ended:
            def remove(ended):
                if self.sources.get(key) is ended:
                    self.sources.pop(key, None)
            source = SharedSource(name, args, remove)
            self.sources[key] = source
            source.task = asyncio.create_task(source.run())
            self.tasks.add(source.task)
            source.task.add_done_callback(self.tasks.discard)
        return source, source.subscribe()

    async def shutdown(self):
        for source in tuple(self.sources.values()):
            source.stop()
        if self.tasks:
            await asyncio.gather(*tuple(self.tasks), return_exceptions=True)
