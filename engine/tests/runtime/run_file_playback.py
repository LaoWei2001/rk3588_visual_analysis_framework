"""Exercise file capture; optionally add a YOLOv8 model and its labels on the board."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

if not shutil.which('ffmpeg'):
    raise SystemExit(77)
with tempfile.TemporaryDirectory(prefix='file-playback-video-') as directory:
    video = Path(directory) / 'sample.mp4'
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi',
                    '-i', 'testsrc2=size=320x240:rate=20', '-t', '20', '-c:v', 'libx264',
                    '-preset', 'ultrafast', '-threads', '1', '-pix_fmt', 'yuv420p', str(video)],
                   check=True, timeout=20)
    env = os.environ.copy()
    for key in ('NOTIFY_SOCKET', 'WATCHDOG_USEC', 'WATCHDOG_PID', 'RK_SHUTDOWN_STATUS_FILE',
                'RK_LOGIC_CONTROL_SOCKET', 'RK_FILE_PLAYBACK_CONTROL_SOCKET', 'RK_DATASET_SOCKET'):
        env.pop(key, None)
    model = os.environ.get('VISION_FILE_TEST_MODEL')
    labels = os.environ.get('VISION_FILE_TEST_LABELS')
    if model and not labels:
        raise SystemExit('VISION_FILE_TEST_MODEL requires VISION_FILE_TEST_LABELS')
    subprocess.run([sys.argv[1], str(video)] + ([model, labels] if model else []),
                   env=env, cwd=directory, check=True, timeout=40)
