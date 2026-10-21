"""Two independent browsers against an isolated RTSP -> HTTP -> MSE chain."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
import urllib.request

import pytest

pw_api = pytest.importorskip('playwright.sync_api')
BACKEND = Path(__file__).resolve().parents[1]
DIST = BACKEND.parent / 'frontend/dist'
urlopen = urllib.request.build_opener(urllib.request.ProxyHandler({})).open

SERVER = r'''
import pathlib, sys, threading, json
import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstRtspServer', '1.0')
from gi.repository import Gst, GstRtspServer, GLib
from contextlib import asynccontextmanager
from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
import uvicorn
from routers import stream

root, dist, http_port, rtsp_port = sys.argv[1:]
root, dist = pathlib.Path(root), pathlib.Path(dist)
assets = root/'demo/assets'
assets.mkdir(parents=True)
(assets/'config.json').write_text(json.dumps({'global': {'enable_rtsp':1, 'rtsp_port':int(rtsp_port)}}))
stream.APPS_ROOT = root
Gst.init(None)
server = GstRtspServer.RTSPServer.new()
server.set_address('127.0.0.1')
server.set_service(rtsp_port)
factory = GstRtspServer.RTSPMediaFactory.new()
factory.set_launch('( videotestsrc is-live=true pattern=smpte ! video/x-raw,format=I420,width=1280,height=1280,framerate=30/1 '
                   '! x264enc tune=zerolatency speed-preset=ultrafast bitrate=4096 key-int-max=30 '
                   '! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 )')
factory.set_shared(True)
server.get_mount_points().add_factory('/live', factory)
assert server.attach(None)
loop = GLib.MainLoop()
threading.Thread(target=loop.run, daemon=True).start()

@asynccontextmanager
async def lifespan(app):
    yield
    await stream._stream_pool.shutdown()
    loop.quit()
app = FastAPI(lifespan=lifespan)
app.include_router(stream.router, prefix='/api')
@app.get('/test/stats')
async def stats():
    return {'sources':len(stream._stream_pool.sources), 'sessions':len(stream._active_streams),
            'pids':[s.process.pid for s in stream._stream_pool.sources.values() if s.process],
            'viewers':[len(s.viewers) for s in stream._stream_pool.sources.values()],
            'queue_bytes':[v.pending_bytes for s in stream._stream_pool.sources.values() for v in s.viewers]}
@app.get('/api/apps')
async def apps(): return [dict(name='demo',status='running',pid=123,config='config.json')]
@app.get('/api/apps/demo/config')
async def config(): return {'global':{'enable_rtsp':1},'channels':[]}
@app.get('/api/apps/demo/logic-actions')
async def controls(): return {'globals':[],'channels':[],'socket_ready':True}
@app.get('/api/apps/demo/file-playback')
async def playback(): return {'sources':[]}
app.mount('/assets', StaticFiles(directory=dist/'assets'))
@app.get('/{path:path}')
async def frontend(path): return FileResponse(dist/'index.html')
uvicorn.run(app, host='127.0.0.1', port=int(http_port), log_level='info')
'''


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


@pytest.fixture
def shared_server(tmp_path):
    http_port, rtsp_port = free_port(), free_port()
    script = tmp_path/'server.py'
    script.write_text(SERVER)
    log_path = tmp_path/'server.log'
    with log_path.open('w') as log:
        proc = subprocess.Popen([sys.executable, str(script), str(tmp_path/'apps'), str(DIST),
                                 str(http_port), str(rtsp_port)], stdout=log, stderr=log,
                                env={**os.environ, 'PYTHONPATH': str(BACKEND)})
        base = f'http://127.0.0.1:{http_port}'
        try:
            for _ in range(80):
                if proc.poll() is not None:
                    pytest.fail(log_path.read_text())
                try:
                    urlopen(base+'/test/stats', timeout=1).close()
                    break
                except OSError:
                    time.sleep(.1)
            else:
                pytest.fail('Isolated server did not start: ' + log_path.read_text())
            yield base, tmp_path
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def test_two_independent_browsers_share_source_across_join_reload_and_departure(shared_server):
    base, output = shared_server
    def stats():
        with urlopen(base+'/test/stats', timeout=2) as response:
            return json.load(response)

    with pw_api.sync_playwright() as pw:
        browsers, pages, requests = [], [], [[], []]
        samples = []
        try:
            for index in range(2):
                browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage', '--no-proxy-server'])
                browsers.append(browser)
                context = browser.new_context(viewport={'width':1400,'height':900})
                context.add_init_script("localStorage.setItem('rk3588-auth',JSON.stringify({state:{token:'isolated',username:'tester'},version:0})); URL.parse ??= (u,b) => {try{return new URL(u,b)}catch{return null}}")
                page = context.new_page()
                pages.append(page)
                page.route_web_socket('**/ws/logs/**', lambda _socket: None)
                page.on('request', lambda request, index=index: requests[index].append(request.url)
                        if '/stream?' in request.url else None)
                page.goto(base+'/live-view')
                page.wait_for_function("document.querySelector('video')?.currentTime > 2", timeout=20000)
            baseline = stats()
            assert baseline['sources'] == 1 and baseline['viewers'] == [2], baseline
            assert len(baseline['pids']) == 1, baseline
            pages[0].evaluate("window.originalVideo=document.querySelector('video')")
            pages[1].reload()
            pages[1].wait_for_function("document.querySelector('video')?.currentTime > 2", timeout=15000)
            # Cross the one-minute mark to catch periodic reconnects and pruning.
            for _ in range(120):
                pages[0].wait_for_timeout(500)
                sample = [page.locator('video').evaluate('''v => ({
                    time:v.currentTime, frames:v.getVideoPlaybackQuality().totalVideoFrames,
                    dropped:v.getVideoPlaybackQuality().droppedVideoFrames, paused:v.paused,
                    error:v.error?.message, ahead:v.buffered.length ? v.buffered.end(v.buffered.length-1)-v.currentTime : 0
                })''') for page in pages]
                samples.append(sample)
                assert all(not value['error'] for value in sample), sample
            for index in range(2):
                assert samples[-1][index]['frames'] - samples[0][index]['frames'] > 100
                for offset in range(4, len(samples)):
                    assert samples[offset][index]['frames'] > samples[offset-4][index]['frames'], samples[offset-4:offset+1]
            assert len(requests[0]) == 1 and len(requests[1]) == 2, requests
            assert pages[0].evaluate("document.querySelector('video') === originalVideo")
            assert stats()['pids'] == baseline['pids']
            before = samples[-1][1]['frames']
            browsers[0].close()
            browsers.pop(0)
            pages[1].wait_for_timeout(2500)
            assert pages[1].locator('video').evaluate('v => v.getVideoPlaybackQuality().totalVideoFrames') > before + 15
            after = stats()
            assert after['pids'] == baseline['pids'] and after['viewers'] == [1], after
            (output/'two-browser-report.json').write_text(json.dumps({'baseline':baseline,'after_departure':after,
                'request_counts':list(map(len,requests)),'samples':samples}, indent=2))
        finally:
            for browser in browsers:
                browser.close()
        time.sleep(3.5)
        assert stats()['sources'] == 0
