"""Measure synthetic live-frame age through RTSP -> actual router -> browser.

Each frame carries the producer's monotonic milliseconds as a 24-bit barcode.
This is transport/playback latency, not a camera-to-physical-display benchmark.
Optional RK3588_TEST_FRONTEND_DIST / RK3588_TEST_STREAM_ROUTER select A/B builds.
RK3588_TEST_ENGINE_BINARY/CONFIG/APP optionally insert the real inference engine
between the timestamp RTSP source and the preview, using a private config copy.
"""
import json
import hashlib
import os
import signal
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
import asyncio, importlib.util, json, os, pathlib, signal, socket, subprocess, sys, threading, time
from contextlib import asynccontextmanager
import gi
gi.require_version('Gst', '1.0')
gi.require_version('GstRtspServer', '1.0')
from gi.repository import Gst, GstRtspServer, GLib
from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
import uvicorn
for key in list(os.environ):
    if key.lower().endswith('_proxy'):os.environ.pop(key,None)

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
engine = None
engine_log = None
direct_probe = None
direct_ages = []
hdmi_ages = []
capture_stop = threading.Event()
capture_thread = None
crop_cols = crop_rows = 1
engine_binary = os.environ.get('RK3588_TEST_ENGINE_BINARY')
if engine_binary:
    app_dir = pathlib.Path(os.environ['RK3588_TEST_ENGINE_APP']).resolve()
    config = json.loads(pathlib.Path(os.environ['RK3588_TEST_ENGINE_CONFIG']).read_text())
    with socket.socket() as port_probe:
        port_probe.bind(('127.0.0.1',0))
        output_port = port_probe.getsockname()[1]
    config['global'].update(enable_display=bool(os.environ.get('RK3588_TEST_HDMI')),enable_rtsp=True,rtsp_codec='h264',rtsp_port=output_port,rtsp_path='/live')
    crop_cols = config['global']['tile_cols']
    crop_rows = config['global']['tile_rows']
    for channel in config['channels']:
        channel['stream'].update(src_type='rtsp',url=f'rtsp://127.0.0.1:{rtsp_port}/live',video_enc='h264')
    engine_config = root/'engine-config.json'
    engine_config.write_text(json.dumps(config,indent=2))
    engine_config.chmod(0o600)
    (assets/'config.json').write_text(json.dumps({'global':{'enable_rtsp':1,'rtsp_port':output_port}}))
    env = dict(os.environ,LD_LIBRARY_PATH=str(app_dir/'libs'),ASSETS_DIR=str(app_dir/'assets'))
    # The fixture's loopback RTSP source must not use the shell's HTTP proxy.
    for key in list(env):
        if key.lower().endswith('_proxy'):env.pop(key,None)
    # Keep UNIX socket paths short and isolated from the installed application's.
    import tempfile
    sockets = tempfile.TemporaryDirectory(prefix='rk-delay-')
    for key,name in [('RK_LOGIC_CONTROL_SOCKET','logic'),('RK_DATASET_SOCKET','dataset'),('RK_FILE_PLAYBACK_CONTROL_SOCKET','playback')]:
        env[key] = str(pathlib.Path(sockets.name)/(name+'.sock'))
    for key in ('NOTIFY_SOCKET','WATCHDOG_PID','WATCHDOG_USEC'):env.pop(key,None)
    engine_log = (root/'engine.log').open('w')
    engine = subprocess.Popen([str(pathlib.Path(engine_binary).resolve()),str(engine_config.resolve())],cwd=app_dir,env=env,stdout=engine_log,stderr=subprocess.STDOUT)
    deadline = time.monotonic()+90
    while time.monotonic()<deadline:
        assert engine.poll() is None,'Engine exited during timestamp test'
        with socket.socket() as probe:
            if probe.connect_ex(('127.0.0.1',output_port))==0:break
        time.sleep(.1)
    else:raise RuntimeError('Engine RTSP startup timeout')
    if os.environ.get('RK3588_TEST_RTSP_PROBE'):
        width,height = config['global']['disp_width'],config['global']['disp_height']
        right = ((width+15)&~15)-((width//crop_cols)&~3)
        bottom = ((height+15)&~15)-((height//crop_rows)&~1)
        direct_probe = Gst.parse_launch(f'rtspsrc location=rtsp://127.0.0.1:{output_port}/live protocols=tcp latency=0 '
            '! rtph264depay ! h264parse ! mppvideodec '
            f'! videocrop right={right} bottom={bottom} ! videoscale ! video/x-raw,width=320,height=180 '
            '! videoconvert ! video/x-raw,format=RGB ! appsink name=age_sink sync=false max-buffers=1 drop=true emit-signals=true')
        def sample_age(sink):
            sample = sink.emit('pull-sample')
            ok,mapped = sample.get_buffer().map(Gst.MapFlags.READ)
            if ok:
                row = mapped.data[40*320*3:41*320*3]
                stamp = 0
                for bit in range(24):stamp = stamp*2+(row[(22+bit*12)*3]>128)
                age = (time.monotonic()*1000-stamp)%16777216
                if stamp:direct_ages.append(age)
                sample.get_buffer().unmap(mapped)
            return Gst.FlowReturn.OK
        direct_probe.get_by_name('age_sink').connect('new-sample',sample_age)
        direct_probe.set_state(Gst.State.PLAYING)
    if os.environ.get('RK3588_TEST_HDMI'):
        # Read the X11 HDMI application's pixels, not a pipeline timestamp.
        # This excludes the monitor's own scanout/pixel response latency.
        def capture_hdmi():
            import ctypes,re
            os.environ.setdefault('DISPLAY',':0')
            os.environ.setdefault('XAUTHORITY','/run/lightdm/root/:0')
            xid = None
            while not capture_stop.wait(.1):
                result = subprocess.run(['xwininfo','-name','rtsp_yolo_grid'],capture_output=True,text=True)
                match = re.search(r'Window id: (0x[0-9a-f]+)',result.stdout)
                if match:xid=int(match[1],16);break
            if xid is None:return
            xlib = ctypes.CDLL('libX11.so.6')
            xlib.XOpenDisplay.argtypes=[ctypes.c_char_p];xlib.XOpenDisplay.restype=ctypes.c_void_p
            xlib.XGetImage.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_int,ctypes.c_int,ctypes.c_uint,ctypes.c_uint,ctypes.c_ulong,ctypes.c_int]
            xlib.XGetImage.restype=ctypes.c_void_p
            xlib.XGetPixel.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_int];xlib.XGetPixel.restype=ctypes.c_ulong
            xlib.XDestroyImage.argtypes=[ctypes.c_void_p]
            xlib.XCloseDisplay.argtypes=[ctypes.c_void_p]
            display=xlib.XOpenDisplay(None)
            if not display:return
            tile_w=(config['global']['disp_width']//crop_cols)&~3
            tile_h=(config['global']['disp_height']//crop_rows)&~1
            try:
                while not capture_stop.wait(.1):
                    pixels=xlib.XGetImage(display,xid,0,int(tile_h*40/180),tile_w,1,ctypes.c_ulong(-1).value,2)
                    if not pixels:continue
                    stamp=0
                    for bit in range(24):
                        pixel=xlib.XGetPixel(pixels,int((22+bit*12)*tile_w/320),0)
                        stamp=stamp*2+(((pixel>>16)&255)>128)
                    xlib.XDestroyImage(pixels)
                    if stamp:hdmi_ages.append((time.monotonic()*1000-stamp)%16777216)
            finally:xlib.XCloseDisplay(display)
        capture_thread=threading.Thread(target=capture_hdmi,daemon=True)
        capture_thread.start()
state = {'paused_until':0}
@asynccontextmanager
async def lifespan(_app):
    yield
    await stream._stream_pool.shutdown()
    if direct_probe:direct_probe.set_state(Gst.State.NULL)
    capture_stop.set()
    if capture_thread:capture_thread.join(3)
    if engine:
        engine.send_signal(signal.SIGINT)
        try:engine.wait(timeout=20)
        except subprocess.TimeoutExpired:
            engine.kill();engine.wait()
        engine_log.close()
        sockets.cleanup()
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
async def clock(): return {'ms':time.monotonic()*1000,'crop_cols':crop_cols,'crop_rows':crop_rows}
@app.get('/test/direct')
async def direct(): return {'frames':len(direct_ages),'last_ages_ms':direct_ages[-120:]}
@app.get('/test/hdmi')
async def hdmi(): return {'frames':len(hdmi_ages),'last_ages_ms':hdmi_ages[-50:]}
@app.post('/test/pause')
async def pause():
    state['paused_until']=time.monotonic()+1.1
    return {'seconds':1.1}
@app.get('/api/apps')
async def apps(): return [dict(name='demo',status='running',pid=1,config='config.json')]
@app.get('/api/apps/demo/config')
async def app_config(): return {'global':{'enable_rtsp':1},'channels':[]}
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
            env={**os.environ, 'PYTHONPATH': str(BACKEND)}, start_new_session=True)
        base = 'http://127.0.0.1:'+str(http_port)
        try:
            for _ in range(900 if os.environ.get('RK3588_TEST_ENGINE_BINARY') else 100):
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
                            window.cropCols=result.crop_cols;window.cropRows=result.crop_rows;
                            const end=performance.now();
                            pairs.push({rtt:end-start,offset:result.ms-(start+end)/2});
                        }
                        pairs.sort((a,b)=>a.rtt-b.rtt);
                        window.clockOffset=pairs[0].offset;
                        window.clockCanvas=document.createElement('canvas');
                        clockCanvas.width=320;clockCanvas.height=180;
                        window.sampleClock=()=>{
                            const v=document.querySelector('video'),c=clockCanvas.getContext('2d');
                            c.drawImage(v,0,0,v.videoWidth/cropCols,v.videoHeight/cropRows,0,0,320,180);
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
                        engine_binary=os.environ.get('RK3588_TEST_ENGINE_BINARY'),
                        boot_id=Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
                        frontend_index_sha256=hashlib.sha256((DIST/'index.html').read_bytes()).hexdigest(),
                        router_sha256=hashlib.sha256(ROUTER.read_bytes()).hexdigest(),
                        forced_http_stalls=3,stall_seconds=1.1,stream_requests=len(requests),
                        before=before,after=after,after_p50_ms=statistics.median(ages),
                        after_p95_ms=ages[int(.95*(len(ages)-1))])
                    if report['engine_binary']:
                        report['engine_sha256']=hashlib.sha256(Path(report['engine_binary']).read_bytes()).hexdigest()
                    if os.environ.get('RK3588_TEST_RTSP_PROBE'):
                        direct = json.loads(urlopen(base+'/test/direct').read())
                        direct_values = sorted(direct['last_ages_ms'])
                        assert len(direct_values)==120, 'Direct RTSP probe did not receive frames'
                        report['direct_rtsp'] = dict(frames=direct['frames'],p50_ms=statistics.median(direct_values),
                            p95_ms=direct_values[int(.95*(len(direct_values)-1))],samples_ms=direct_values)
                    if os.environ.get('RK3588_TEST_HDMI'):
                        hdmi = json.loads(urlopen(base+'/test/hdmi').read())
                        hdmi_values = sorted(hdmi['last_ages_ms'])
                        assert len(hdmi_values)==50, 'HDMI window probe did not receive frames'
                        report['hdmi_window'] = dict(frames=hdmi['frames'],p50_ms=statistics.median(hdmi_values),
                            p95_ms=hdmi_values[int(.95*(len(hdmi_values)-1))],samples_ms=hdmi_values)
                    (tmp_path/'latency-report.json').write_text(json.dumps(report,indent=2))
                    page.locator('video').screenshot(path=str(tmp_path/'video.png'))
                    summary={k:v for k,v in report.items() if k not in ('before','after')}
                    for key in ('direct_rtsp','hdmi_window'):
                        if key in summary:summary[key]={k:v for k,v in summary[key].items() if k!='samples_ms'}
                    print('LATENCY',json.dumps(summary),flush=True)
                    assert after[-1]['frames']>after[0]['frames']+30
                    assert len(requests)==1, 'Short jitter should recover without reopening the stream'
                    assert report['after_p95_ms']<float(os.environ.get('RK3588_TEST_MAX_LIVE_LATENCY_MS','1200')),report
                finally:
                    browser.close()
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=30 if os.environ.get('RK3588_TEST_ENGINE_BINARY') else 8)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid,signal.SIGKILL)
                proc.wait()
            finally:
                # Also clean up a child engine if fixture startup failed before
                # uvicorn installed its lifespan/signal handlers.
                try:os.killpg(proc.pid,signal.SIGTERM)
                except ProcessLookupError:pass
