"""Measure synthetic live-frame age through RTSP -> actual router -> browser.

Each frame carries the producer's monotonic milliseconds as a 24-bit barcode.
This is transport/playback latency, not a camera-to-physical-display benchmark.
Optional RK3588_TEST_FRONTEND_DIST / RK3588_TEST_STREAM_ROUTER select A/B builds.
"""
import json
import os
from pathlib import Path
import socket
import statistics
import subprocess
import sys
import time
import urllib.request

import pytest

pw_api = pytest.importorskip('playwright.sync_api')
BACKEND = Path(__file__).resolve().parents[1]
DIST = Path(os.environ.get('RK3588_TEST_FRONTEND_DIST', BACKEND.parent/'frontend/dist'))
ROUTER = Path(os.environ.get('RK3588_TEST_STREAM_ROUTER', BACKEND/'routers/stream.py'))
urlopen = urllib.request.build_opener(urllib.request.ProxyHandler({})).open

SERVER = r'''
import asyncio, importlib.util, json, pathlib, sys, threading, time
from contextlib import asynccontextmanager
import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstRtspServer', '1.0')
from gi.repository import Gst, GstRtspServer, GLib
from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
import uvicorn

root, dist, router_file, http_port, rtsp_port = sys.argv[1:]
root, dist = pathlib.Path(root), pathlib.Path(dist)
spec = importlib.util.spec_from_file_location('latency_stream', router_file)
stream = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = stream
spec.loader.exec_module(stream)
assets = root/'demo/assets'
assets.mkdir(parents=True)
(assets/'config.json').write_text(json.dumps({'global':{'enable_rtsp':1,'rtsp_port':int(rtsp_port)}}))
stream.APPS_ROOT = root
Gst.init(None)
server = GstRtspServer.RTSPServer.new()
server.set_address('127.0.0.1')
server.set_service(rtsp_port)
factory = GstRtspServer.RTSPMediaFactory.new()
factory.set_launch('( appsrc name=clock_source is-live=true block=false do-timestamp=true format=time '
    'caps=video/x-raw,format=RGB,width=320,height=180,framerate=30/1 '
    '! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream '
    '! videoconvert ! x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 bitrate=300 '
    '! h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 )')
factory.set_shared(True)
def configure(_factory, media):
    source = media.get_element().get_by_name('clock_source')
    alive = [True]
    media.connect('unprepared', lambda _media: alive.__setitem__(0,False))
    def push():
        if not alive[0]: return False
        stamp = int(time.monotonic()*1000) & 0xffffff
        row = bytearray([32]*320*3)
        for bit in range(24):
            value = 235 if (stamp >> (23-bit)) & 1 else 16
            begin = (16+bit*12)*3
            row[begin:begin+36] = bytes([value])*36
        data = bytes(row)*180
        buffer = Gst.Buffer.new_allocate(None,len(data),None)
        buffer.fill(0,data)
        buffer.duration = Gst.SECOND//30
        return source.emit('push-buffer',buffer) == Gst.FlowReturn.OK
    GLib.timeout_add(33,push)
factory.connect('media-configure',configure)
server.get_mount_points().add_factory('/live',factory)
assert server.attach(None)
loop = GLib.MainLoop()
threading.Thread(target=loop.run,daemon=True).start()
state = {'paused_until':0}
@asynccontextmanager
async def lifespan(_app):
    yield
    await stream._stream_pool.shutdown()
    loop.quit()
app = FastAPI(lifespan=lifespan)
class NetworkPause:
    def __init__(self,app): self.app=app
    async def __call__(self,scope,receive,send):
        async def paced(message):
            if scope.get('path','').endswith('/stream') and message['type']=='http.response.body':
                delay = state['paused_until']-time.monotonic()
                if delay>0: await asyncio.sleep(delay)
            await send(message)
        await self.app(scope,receive,paced)
app.add_middleware(NetworkPause)
app.include_router(stream.router,prefix='/api')
@app.get('/test/clock')
async def clock(): return {'ms':time.monotonic()*1000}
@app.post('/test/pause')
async def pause():
    state['paused_until']=time.monotonic()+1.1
    return {'seconds':1.1}
@app.get('/api/apps')
async def apps(): return [dict(name='demo',status='running',pid=1,config='config.json')]
@app.get('/api/apps/demo/config')
async def config(): return {'global':{'enable_rtsp':1},'channels':[]}
@app.get('/api/apps/demo/logic-actions')
async def actions(): return {'globals':[],'channels':[],'socket_ready':True}
@app.get('/api/apps/demo/file-playback')
async def playback(): return {'sources':[]}
app.mount('/assets',StaticFiles(directory=dist/'assets'))
@app.get('/{path:path}')
async def frontend(path): return FileResponse(dist/'index.html')
uvicorn.run(app,host='127.0.0.1',port=int(http_port),log_level='info')
'''


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def test_frame_age_returns_to_live_after_network_stalls(tmp_path):
    http_port, rtsp_port = free_port(), free_port()
    script = tmp_path/'server.py'
    script.write_text(SERVER)
    with (tmp_path/'server.log').open('w') as log:
        proc = subprocess.Popen([sys.executable, str(script), str(tmp_path/'apps'),
            str(DIST), str(ROUTER), str(http_port), str(rtsp_port)], stdout=log, stderr=log,
            env={**os.environ, 'PYTHONPATH': str(BACKEND)})
        base = 'http://127.0.0.1:'+str(http_port)
        try:
            for _ in range(100):
                assert proc.poll() is None, (tmp_path/'server.log').read_text()
                try:
                    urlopen(base+'/test/clock', timeout=1).close()
                    break
                except OSError:
                    time.sleep(.1)
            else:
                pytest.fail('Timestamp RTSP fixture did not start')
            with pw_api.sync_playwright() as pw:
                browser = pw.chromium.launch(
                    executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                    headless=True, args=['--no-sandbox', '--disable-dev-shm-usage', '--no-proxy-server'])
                try:
                    context = browser.new_context(viewport={'width':1200, 'height':800})
                    context.add_init_script("localStorage.setItem('rk3588-auth',JSON.stringify({state:{token:'isolated',username:'tester'},version:0})); URL.parse ??= (u,b)=>{try{return new URL(u,b)}catch{return null}}")
                    page = context.new_page()
                    page.route_web_socket('**/ws/logs/**', lambda _socket: None)
                    requests = []
                    page.on('request', lambda r: requests.append(r.url) if '/stream?' in r.url else None)
                    started = time.monotonic()
                    page.goto(base+'/live-view')
                    page.wait_for_function("document.querySelector('video')?.getVideoPlaybackQuality().totalVideoFrames>5",timeout=20000)
                    startup = time.monotonic()-started
                    page.evaluate("""async () => {
                        const pairs = [];
                        for(let i=0;i<8;i++) {
                            const start=performance.now();
                            const result=await fetch('/test/clock').then(r=>r.json());
                            const end=performance.now();
                            pairs.push({rtt:end-start,offset:result.ms-(start+end)/2});
                        }
                        pairs.sort((a,b)=>a.rtt-b.rtt);
                        window.clockOffset=pairs[0].offset;
                        window.clockCanvas=document.createElement('canvas');
                        clockCanvas.width=320;clockCanvas.height=180;
                        window.sampleClock=()=>{
                            const v=document.querySelector('video'),c=clockCanvas.getContext('2d');
                            c.drawImage(v,0,0,320,180);
                            const row=c.getImageData(0,40,320,1).data;
                            let stamp=0;
                            for(let i=0;i<24;i++) stamp=stamp*2+(row[(22+i*12)*4]>128?1:0);
                            return {age_ms:((performance.now()+clockOffset-stamp)%16777216+16777216)%16777216,
                                ahead:v.buffered.length?v.buffered.end(v.buffered.length-1)-v.currentTime:0,
                                frames:v.getVideoPlaybackQuality().totalVideoFrames,paused:v.paused,error:v.error?.message};
                        };
                    }""")
                    before = []
                    for _ in range(20):
                        page.wait_for_timeout(100)
                        before.append(page.evaluate('sampleClock()'))
                    # Same continuous media timeline, briefly blocked HTTP delivery.
                    # Repeat early stalls to reproduce the reported permanent lag.
                    for _ in range(3):
                        urlopen(urllib.request.Request(base+'/test/pause', method='POST'),timeout=2).close()
                        page.wait_for_timeout(3200)
                    page.wait_for_timeout(10000)
                    after = []
                    for _ in range(50):
                        page.wait_for_timeout(100)
                        after.append(page.evaluate('sampleClock()'))
                    assert all(not x.get('error') for x in before+after)
                    ages = sorted(x['age_ms'] for x in after)
                    report = dict(startup_seconds=startup,source='320x180@30 synthetic monotonic barcode',
                        forced_http_stalls=3,stall_seconds=1.1,stream_requests=len(requests),
                        before=before,after=after,after_p50_ms=statistics.median(ages),
                        after_p95_ms=ages[int(.95*(len(ages)-1))])
                    (tmp_path/'latency-report.json').write_text(json.dumps(report,indent=2))
                    print('LATENCY',json.dumps({k:v for k,v in report.items() if k not in ('before','after')}),flush=True)
                    assert after[-1]['frames']>after[0]['frames']+30
                    assert len(requests)==1, 'Short jitter should recover without reopening the stream'
                    assert report['after_p95_ms']<float(os.environ.get('RK3588_TEST_MAX_LIVE_LATENCY_MS','1200')),report
                finally:
                    browser.close()
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
