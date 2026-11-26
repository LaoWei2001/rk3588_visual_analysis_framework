"""Opt-in board test: real NPU inference with simulated statvfs exhaustion.

Set IMAGE_INFERENCE_TEST_MODEL and IMAGE_INFERENCE_TEST_LABELS to board assets.
No filesystem is filled; the fault is injected only into this subprocess.
"""
import json
import os
from pathlib import Path
import struct
import subprocess

import pytest

from services.image_inference.engine import engine_environment, npu_busy


@pytest.mark.skipif(not os.environ.get('IMAGE_INFERENCE_TEST_MODEL'), reason='Requires RK3588 test model')
def test_native_storage_pause_resume_keeps_completed_outputs(tmp_path):
    if npu_busy():
        pytest.skip('NPU is in use')
    repo = Path(__file__).resolve().parents[3]
    runner = repo / 'build/engine/image_inference_runner'
    runtime = Path('/opt/ai_apps/_console/inference_runtime')
    # Native storage checks must produce a resumable status even if the Python
    # monitor has not seen the low-space condition yet.
    fault = tmp_path / 'disk_fault.c'
    fault.write_text(r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
int statvfs(const char *path, struct statvfs *info) {
    int (*original)(const char *, struct statvfs *) = dlsym(RTLD_NEXT, "statvfs");
    int result = original(path, info);
    const char *checkpoint = getenv("TEST_DISK_FULL_AFTER_SUCCESS");
    if (!result && checkpoint) {
        FILE *file = fopen(checkpoint, "r");
        if (file) {
            char text[8192] = {0}; fread(text, 1, sizeof(text)-1, file); fclose(file);
            if (strstr(text, "\"status\":\"success\""))
                info->f_bavail = 100ULL * 1024 * 1024 / info->f_frsize;
        }
    }
    return result;
}
''')
    library = tmp_path / 'disk_fault.so'
    subprocess.run(['cc', '-shared', '-fPIC', str(fault), '-ldl', '-o', str(library)], check=True)
    image = tmp_path / 'image.bmp'
    pixels = b'\x30\x60\x90' * (32 * 32)
    image.write_bytes(struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54) +
                     struct.pack('<IiiHHIIiiII', 40, 32, 32, 1, 24, 0, len(pixels), 0, 0, 0, 0) + pixels)
    output = tmp_path / 'output'
    job = dict(models=[dict(id='test', model_type='yolov8_det',
                           model_path=os.environ['IMAGE_INFERENCE_TEST_MODEL'],
                           label_path=os.environ['IMAGE_INFERENCE_TEST_LABELS'],
                           obj_thresh=.3, nms_thresh=.45, npu_core=-1, detect_classes=[])],
               output_dir=str(output), images=[dict(path=str(image), name=f'{i}.bmp') for i in range(3)])
    job_file = tmp_path / 'job.json'
    job_file.write_text(json.dumps(job))
    env = engine_environment(runtime)
    env.update(LD_PRELOAD=str(library), TEST_DISK_FULL_AFTER_SUCCESS=str(output / 'progress.json'))
    def run():
        process = subprocess.run([str(runner), '--infer-images', str(job_file)], env=env,
                                 capture_output=True, text=True, timeout=60)
        assert process.returncode in (0, 2), process.stdout + process.stderr
        return json.loads((output / 'progress.json').read_text())
    paused = run()
    assert paused['status'] == 'paused_storage', paused
    assert [i['status'] for i in paused['items']] == ['success', 'running', 'pending']
    completed_files = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in (output / '000000').iterdir()}
    job['resume'] = True
    job_file.write_text(json.dumps(job))
    assert run()['status'] == 'paused_storage'  # Still full: checkpoint remains retryable.
    env.pop('TEST_DISK_FULL_AFTER_SUCCESS')
    completed = run()
    assert completed['status'] == 'completed', completed
    assert [i['status'] for i in completed['items']] == ['success'] * 3
    assert {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in completed_files} == completed_files
