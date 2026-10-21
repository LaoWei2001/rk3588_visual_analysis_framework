"""Play real H264/fMP4 while logs arrive, the tab returns, and transport recovers."""

from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import threading
import time
from urllib.parse import parse_qs, urlsplit

import pytest

pw_api = pytest.importorskip('playwright.sync_api')
DIST = Path(os.environ.get('RK3588_TEST_FRONTEND_DIST', Path(__file__).resolve().parents[1] / 'dist'))


@pytest.fixture(scope='module')
def fragments(tmp_path_factory):
    path = tmp_path_factory.mktemp('live-video') / 'sample.mp4'
    subprocess.run([
        'ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
        'testsrc2=size=320x180:rate=10', '-t', '60', '-an', '-c:v', 'libx264',
        '-preset', 'ultrafast', '-tune', 'zerolatency', '-g', '10', '-pix_fmt', 'yuv420p',
        '-movflags', 'empty_moov+default_base_moof', '-frag_duration', '200000', str(path),
    ], check=True, timeout=30)
    data = path.read_bytes()
    parts, current, offset = [], bytearray(), 0
    while offset < len(data):
        size = int.from_bytes(data[offset:offset + 4], 'big')
        assert size >= 8
        if data[offset + 4:offset + 8] == b'moof' and current:
            parts.append(bytes(current))
            current.clear()
        current.extend(data[offset:offset + size])
        offset += size
    if current:
        parts.append(bytes(current))
    assert len(parts) > 200
    return parts


@pytest.fixture
def playback(fragments):
    state = dict(requests=0, paused=False, drop=False, queries=[])
    stopped = threading.Event()

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(DIST), **kwargs)

        def do_GET(self):
            path = urlsplit(self.path).path
            if path == '/api/apps/demo/stream':
                state['requests'] += 1
                state['queries'].append(parse_qs(urlsplit(self.path).query))
                self.send_response(200)
                self.send_header('Content-Type', 'video/mp4')
                self.send_header('Cache-Control', 'no-store')
                self.end_headers()
                due = time.monotonic()
                for index, part in enumerate(fragments):
                    if index:
                        due += .2
                    if stopped.wait(max(0, due - time.monotonic())):
                        return
                    while state['paused'] and not stopped.wait(.02):
                        pass
                    if stopped.is_set() or state['drop']:
                        state['drop'] = False
                        return
                    try:
                        self.wfile.write(part)
                        self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError):
                        return
                return
            responses = {
                '/api/apps': [dict(name='demo', status='running', pid=123, config='config.json')],
                '/api/apps/demo/config': {'global': {'enable_rtsp': 1}, 'channels': []},
                '/api/apps/demo/logic-actions': {'globals': [], 'channels': [], 'socket_ready': True},
                '/api/apps/demo/file-playback': {'sources': []},
            }
            if path in responses:
                body = json.dumps(responses[path]).encode()
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if not path.startswith('/assets/'):
                self.path = '/index.html'
            super().do_GET()

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with pw_api.sync_playwright() as pw:
            browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
            context = browser.new_context(viewport={'width': 1500, 'height': 1000})
            context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'isolated',username:'tester'},version:0})); URL.parse ??= (url, base) => { try { return new URL(url, base) } catch { return null } }")
            page = context.new_page()
            errors, sockets = [], []
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.route_web_socket('**/ws/logs/**', lambda socket: sockets.append(socket))
            page.goto(f'http://127.0.0.1:{server.server_port}/live-view')
            page.wait_for_function("document.querySelector('video')?.currentTime > 1", timeout=15000)
            yield page, sockets, state
            assert not errors, errors
            browser.close()
    finally:
        stopped.set()
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


def test_video_keeps_connection_through_logs_visibility_clock_and_quota_pressure(playback):
    page, sockets, state = playback
    page.evaluate('''() => {
        window.initialVideo = document.querySelector('video');
        window.initialFrameCount = initialVideo.getVideoPlaybackQuality().totalVideoFrames;
        window.seeks = 0;
        initialVideo.addEventListener('seeking', () => window.seeks++);
        window.quotaInjected = false;
        const append = SourceBuffer.prototype.appendBuffer;
        SourceBuffer.prototype.appendBuffer = function(bytes) {
            if (!window.quotaInjected && initialVideo.currentTime > 4) {
                window.quotaInjected = true;
                throw new DOMException('Simulated full media buffer', 'QuotaExceededError');
            }
            return append.call(this, bytes);
        };
    }''')
    for number in range(80):
        sockets[-1].send('\n'.join(f'load-{number}-{i} ' + 'x' * 150 for i in range(50)) + '\n')
        page.wait_for_timeout(5)
    pw_api.expect(page.locator('.live-view-log-line').last).to_contain_text('load-79-49')
    page.evaluate("Object.defineProperty(document, 'hidden', {configurable:true,value:true}); document.dispatchEvent(new Event('visibilitychange'))")
    page.wait_for_timeout(1300)
    page.evaluate("Object.defineProperty(document, 'hidden', {configurable:true,value:false}); document.dispatchEvent(new Event('visibilitychange'))")
    state['paused'] = True
    page.wait_for_timeout(1000)
    page.evaluate('''() => { const now = Date.now; Date.now = () => now() + 3600000 }''')
    page.wait_for_timeout(1200)
    state['paused'] = False
    assert state['requests'] == 1
    # Cross the buffer-pruning threshold too, with real decode/playback continuing.
    page.wait_for_function('initialVideo.currentTime > 16', timeout=20000)
    assert state['requests'] == 1
    assert page.evaluate('window.quotaInjected')
    assert page.evaluate("document.querySelector('video') === initialVideo")
    assert page.evaluate('initialVideo.getVideoPlaybackQuality().totalVideoFrames > initialFrameCount + 30')
    assert page.evaluate('window.seeks') < 5
    assert page.locator('video').evaluate('e => !e.error && getComputedStyle(e).visibility === "visible"')


def test_real_disconnect_preserves_frame_and_recovers(playback):
    page, _, state = playback
    state['drop'] = True
    snapshot = page.locator('.live-view-video-snapshot')
    pw_api.expect(snapshot).to_be_visible(timeout=5000)
    assert snapshot.evaluate('e => e.width > 0 && e.height > 0')
    pw_api.expect(page.get_by_text('连接恢复中，暂时显示上一帧……', exact=True)).to_be_visible()
    page.wait_for_function('''() => {
        const v = document.querySelector('video');
        return v && v.currentTime > 1 && document.querySelector('.live-view-video-snapshot').hidden;
    }''', timeout=10000)
    assert state['requests'] == 2
    assert state['queries'][-1]['client_id'] == state['queries'][0]['client_id']
    assert state['queries'][-1]['retry_cause'] == ['transport-error']
    assert int(state['queries'][-1]['retry_position_ms'][0]) > 0
    assert 'retry_ahead_ms' in state['queries'][-1]


def test_starvation_accumulates_reserve_without_reconnect(playback):
    page, _, state = playback
    page.evaluate('''() => {
        window.starvedVideo = document.querySelector('video');
        window.resumeAhead = null;
        starvedVideo.addEventListener('playing', () => {
            if (starvedVideo.buffered.length) window.resumeAhead =
                starvedVideo.buffered.end(starvedVideo.buffered.length - 1) - starvedVideo.currentTime;
        });
    }''')
    state['paused'] = True
    page.wait_for_function('starvedVideo.paused && starvedVideo.readyState < 3', timeout=6000)
    position = page.evaluate('starvedVideo.currentTime')
    state['paused'] = False
    page.wait_for_function('window.resumeAhead !== null && starvedVideo.currentTime > 4', timeout=8000)
    assert page.evaluate('window.resumeAhead') >= .9
    assert state['requests'] == 1
    assert page.evaluate('document.querySelector("video") === starvedVideo && !starvedVideo.error')
    assert page.evaluate('starvedVideo.currentTime') > position


def test_buffer_operation_that_never_finishes_recovers(playback):
    page, _, state = playback
    page.evaluate('''() => {
        const append = SourceBuffer.prototype.appendBuffer;
        let blocked = false;
        SourceBuffer.prototype.appendBuffer = function(bytes) {
            if (!blocked) { blocked = true; return; }
            return append.call(this, bytes);
        };
        window.blockedVideo = document.querySelector('video');
    }''')
    page.wait_for_function('''() => {
        const video = document.querySelector('video');
        return video && video !== blockedVideo && video.currentTime > 1 &&
            document.querySelector('.live-view-video-snapshot').hidden;
    }''', timeout=12000)
    assert state['requests'] == 2
    assert state['queries'][-1]['retry_cause'] == ['buffer-timeout']


def test_short_recoveries_keep_backoff_until_frames_are_stable(playback):
    page, _, state = playback
    for attempt in range(1, 4):
        page.evaluate("window.previousVideo = document.querySelector('video')")
        state['drop'] = True
        page.wait_for_function('''() => {
            const video = document.querySelector('video');
            return video && video !== previousVideo && video.currentTime > 1 &&
                document.querySelector('.live-view-video-snapshot').hidden;
        }''', timeout=10000)
        query = state['queries'][-1]
        assert query['retry_attempt'] == [str(attempt)]
        delay = int(query['retry_delay_ms'][0])
        assert .75 * 500 * 2 ** (attempt - 1) <= delay <= 500 * 2 ** (attempt - 1)
    page.wait_for_function("document.querySelector('video').currentTime > 12", timeout=16000)
    page.evaluate("window.previousVideo = document.querySelector('video')")
    state['drop'] = True
    page.wait_for_function('''() => {
        const video = document.querySelector('video');
        return video && video !== previousVideo && video.currentTime > 1 &&
            document.querySelector('.live-view-video-snapshot').hidden;
    }''', timeout=10000)
    assert state['queries'][-1]['retry_attempt'] == ['1']
