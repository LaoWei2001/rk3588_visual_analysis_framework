"""RK3588: original-size preview, DMA strides, shared leases and decoded colors.

Run with: python3 test_source_preview.py /path/to/vision_analysis
Requires FFmpeg, RGA and the MPP H264 encoder; no camera or model needed.
"""
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

def command(path, action, channel_id=0, lease='test'):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(10)
        client.connect(str(path))
        client.sendall(json.dumps(dict(scope='preview', action=action,
                                      channel_id=channel_id, lease=lease)).encode() + b'\n')
        data = b''
        while not data.endswith(b'\n'):
            part = client.recv(4096)
            assert part, 'control socket closed before reply'
            data += part
        return json.loads(data)


def main(binary):
    binary = str(Path(binary).resolve())
    dimensions = [(854, 480), (1920, 1080), (3840, 2160)]
    with tempfile.TemporaryDirectory(prefix='source-preview-') as temp:
        root = Path(temp)
        with socket.socket() as free_port:
            free_port.bind(('127.0.0.1', 0))
            port = free_port.getsockname()[1]
        channels = []
        for index, (width, height) in enumerate(dimensions):
            video = root / f'{index}.mp4'
            subprocess.run([
                'ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
                f'color=red:size={width}x{height}:rate=10', '-vf',
                f'drawbox=x={width//2}:y=0:w={width}:h={height//2}:color=lime:t=fill,'
                f'drawbox=x=0:y={height//2}:w={width//2}:h={height}:color=blue:t=fill,'
                f'drawbox=x={width//2}:y={height//2}:w={width}:h={height}:color=yellow:t=fill',
                '-t', '2', '-an', '-c:v', 'libx264', '-threads', '2', '-preset', 'ultrafast',
                '-pix_fmt', 'yuv420p', str(video),
            ], check=True, timeout=30)
            channels.append(dict(id=index, enable=True, models=[], max_fps=10,
                                 stream=dict(src_type='file', url=str(video), loop=True), logic='',
                                 report_policy=dict(enabled=False, deliveries=[])))
        config = root / 'config.json'
        config.write_text(json.dumps({'global': dict(enable_display=False, enable_rtsp=True,
            rtsp_port=port, rtsp_path='/preview', rtsp_codec='h264', rtsp_encoder='hw',
            disp_width=320, disp_height=180, tile_rows=2, tile_cols=2, tracker_enable=0,
            performance_display=False, global_logics=[]), 'channels': channels}))
        control = root / 'control.sock'
        with (root / 'engine.log').open('w+') as log:
            process = subprocess.Popen([binary, str(config)], cwd=root,
                env={**os.environ, 'RK_LOGIC_CONTROL_SOCKET': str(control)}, stdout=log, stderr=log)
            try:
                for index, expected_size in enumerate(dimensions):
                    deadline = time.monotonic() + 20
                    while True:
                        assert process.poll() is None, 'engine exited'
                        try:
                            response = command(control, 'acquire', index)
                            if response.get('ok'):
                                break
                        except (OSError, ValueError):
                            pass
                        assert time.monotonic() < deadline, 'preview never became available'
                        time.sleep(.1)
                    assert (response['width'], response['height']) == expected_size, response
                    assert 'preview_resolutions' not in response
                    second = command(control, 'acquire', index, 'second-viewer')
                    assert second['path'] == response['path'] == f'/preview/channel/{index}'
                    assert command(control, 'release', index)['ok']
                    url = f"rtsp://127.0.0.1:{port}{response['path']}"
                    image = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error',
                        '-rtsp_transport', 'tcp', '-i', url, '-frames:v', '1', '-threads', '1',
                        '-f', 'image2pipe', '-vcodec', 'ppm', '-'],
                        capture_output=True, check=True, timeout=20)
                    magic, size, depth, pixels = image.stdout.split(b'\n', 3)
                    assert magic == b'P6' and depth == b'255'
                    width, height = map(int, size.split())
                    assert (width, height) == expected_size, ((width, height), expected_size)
                    assert len(pixels) == width * height * 3
                    for x, y, color in [(4, 4, (255, 0, 0)), (width-5, 4, (0, 255, 0)),
                                        (4, height-5, (0, 0, 255)), (width-5, height-5, (255, 255, 0))]:
                        offset = (y * width + x) * 3
                        actual = pixels[offset:offset+3]
                        assert all(abs(a-b) < 35 for a, b in zip(actual, color)), (expected_size, x, y, actual)
                    assert command(control, 'release', index, 'second-viewer')['ok']
                    print(f'PASS: {width}x{height}, original dimensions, correct edge colors, shared stream', flush=True)
            except Exception:
                log.flush()
                log.seek(0)
                print(log.read()[-16000:], file=sys.stderr)
                raise
            finally:
                process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            assert process.returncode == 0, process.returncode


if __name__ == '__main__':
    main(sys.argv[1])
