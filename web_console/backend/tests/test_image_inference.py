from io import BytesIO
import errno
import json
from pathlib import Path
import time
import zipfile

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from services.image_inference import engine, jobs, sources
from services.image_inference.assets import ModelAssets
from routers import image_inference as routes


# A process fixture exercises the real subprocess/progress/lifecycle adapter.
# Inference correctness belongs to the C++ tests and board integration checks.
FAKE_ENGINE = '''#!/usr/bin/python3
import json, pathlib, signal, sys, time
if sys.argv[1] == '--image-inference-capabilities':
 print('IMAGE_INFERENCE_CAPABILITIES=' + json.dumps(dict(version=1, models=[dict(type=t, requires_labels=True, result_kind='detection') for t in ['yolov8_det', 'yolov5']])))
 sys.exit(0)
if sys.argv[1] == '--inspect-model':
 model = pathlib.Path(sys.argv[2])
 with (pathlib.Path(__file__).parent/'probes.log').open('a') as log: print(model.name, file=log)
 if model.read_bytes().startswith(b'broken'):
  print('MODEL_INFO_JSON=' + json.dumps(dict(ok=False,error='RKNN 模型无法加载')));sys.exit(1)
 outputs=[dict(format='NCHW',shape=[1,c,640//stride,640//stride]) for stride in (8,16,32) for c in (64,2,1)]
 print('MODEL_INFO_JSON=' + json.dumps(dict(ok=True,inspection_version=1,width=640,height=640,format='NHWC',outputs=outputs,compatibility=dict(yolov8_det=dict(ok=True,error=''),yolov5=dict(ok=False,error='yolov5 当前要求 3 个输出张量')))))
 sys.exit(0)
job = json.loads(pathlib.Path(sys.argv[2]).read_text())
out = pathlib.Path(job['output_dir']); out.mkdir()
items = [dict(index=i,name=x['name'],status='pending') for i,x in enumerate(job['images'])]
def progress(status):
 p=out/'progress.json.tmp';p.write_text(json.dumps(dict(status=status,items=items)));p.replace(out/'progress.json')
def cancel(*args):
 progress('cancelled');sys.exit(130)
signal.signal(signal.SIGINT, cancel)
progress('running')
if any(x['name'].startswith('slow') for x in job['images']): time.sleep(30)
failures=0
for i, image in enumerate(job['images']):
 if pathlib.Path(image['path']).read_bytes()==b'bad':
  items[i].update(status='failed',error='Cannot decode image');failures+=1
 else:
  directory=out/f'{i:06d}';directory.mkdir()
  (directory/'result.json').write_text(json.dumps(dict(models=job['models'],results=[])))
  (directory/'original.jpg').write_bytes(b'original')
  (directory/'annotated.jpg').write_bytes(b'jpeg')
  items[i].update(status='success',result=f'{i:06d}/result.json')
 progress('running')
progress('completed_with_errors' if failures else 'completed')
sys.exit(1 if failures else 0)
'''


@pytest.fixture
def console(tmp_path, monkeypatch):
    runtime = tmp_path / 'runtime'
    runtime.mkdir()
    assets = tmp_path / 'models'
    assets.mkdir(parents=True)
    (assets / 'model.rknn').write_bytes(b'model')
    (assets / 'labels.txt').write_text('person\n')
    binary = runtime / 'image_inference_runner'
    binary.write_text(FAKE_ENGINE)
    binary.chmod(0o700)
    monkeypatch.setattr(engine, 'NPU_LOCK_FILE', tmp_path / 'npu.lock')
    catalog = engine.EngineCatalog(runtime)
    manager = jobs.ImageJobs(tmp_path / 'jobs', catalog)
    monkeypatch.setattr(routes, 'catalog', catalog)
    monkeypatch.setattr(routes, 'console_root', tmp_path / 'apps/_console')
    monkeypatch.setattr(routes, 'model_assets', ModelAssets(tmp_path / 'library'))
    monkeypatch.setattr(routes, 'image_jobs', manager)
    server = FastAPI()
    server.include_router(routes.router, prefix='/api')
    with TestClient(server) as client:
        yield client, manager, catalog
    manager.shutdown()


def settings(catalog):
    assets = catalog.runtime.parent / 'models'
    common = dict(model_type='yolov8_det', model_path=str(assets / 'model.rknn'), label_path=str(assets / 'labels.txt'), nms_thresh=.45)
    return dict(models=[dict(common, id='person', obj_thresh=.9), dict(common, id='secondary', obj_thresh=.2)])


BASE = '/api/image-inference'


def wait(manager, job_id):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        result = manager.current()
        assert result is not None and result['id'] == job_id
        if result['status'] in jobs.TERMINAL:
            return result
        time.sleep(.03)
    raise AssertionError(manager.current())


def test_batch_multi_model_partial_failure_and_restart(console):
    client, manager, catalog = console
    response = client.post(BASE + '/jobs', data={'settings': json.dumps(settings(manager.catalog))}, files=[
        ('files', ('same.jpg', b'ok', 'image/jpeg')), ('files', ('same.jpg', b'bad', 'image/jpeg')),
        ('files', ('last.png', b'ok', 'image/png'))])
    assert response.status_code == 202, response.text
    job_id = response.json()['id']
    result = wait(manager, job_id)
    assert result['status'] == 'completed_with_errors'
    assert [i['status'] for i in result['items']] == ['success', 'failed', 'success']
    details = client.get(f'{BASE}/jobs/{job_id}/items/0').json()
    assert [m['obj_thresh'] for m in details['models']] == [.9, .2]
    assert [m['id'] for m in details['models']] == ['person', 'secondary']
    download = client.get(f'{BASE}/jobs/{job_id}/download')
    assert download.status_code == 200
    with zipfile.ZipFile(BytesIO(download.content)) as archive:
        assert set(archive.namelist()) == {'000000/original.jpg', '000000/annotated.jpg',
                                         '000002/original.jpg', '000002/annotated.jpg'}
    recovered = jobs.ImageJobs(manager.root, catalog)
    assert recovered.current()['status'] == 'completed_with_errors'
    assert client.get(f'{BASE}/jobs/{job_id}/files/000000/annotated.jpg').status_code == 200
    assert client.get(f'{BASE}/jobs/{job_id}/files/000000/original.jpg?download=true').content == b'original'
    assert client.get(f'{BASE}/jobs/{job_id}/files/000000/result.json').status_code == 404
    assert client.get(f'{BASE}/jobs/{job_id}/files/000000/result.json?download=true').status_code == 404
    assert client.get(f'{BASE}/jobs/{job_id}/files/000000/engine.log').status_code == 404
    with pytest.raises(engine.JobError):
        manager.artifact(job_id, '../job.json')


def test_model_inspection_cache_type_switch_and_submission_checks(console):
    client, manager, catalog = console
    options = settings(catalog)
    path = options['models'][0]['model_path']
    query = dict(path=path, model_type='yolov8_det')
    checked = client.get(BASE + '/models/info', params=query)
    assert checked.status_code == 200, checked.text
    assert checked.json()['ok'] and checked.json()['width'] == checked.json()['height'] == 640
    assert checked.json()['format'] == 'NHWC' and checked.json()['path'] == path
    mismatch = client.get(BASE + '/models/info', params=dict(query, model_type='yolov5')).json()
    assert not mismatch['ok'] and '3 个输出张量' in mismatch['error']
    options['models'][0]['model_type'] = 'yolov5'
    rejected = client.post(BASE + '/jobs', data={'settings': json.dumps(options)}, files={'files': ('x.jpg', b'ok')})
    assert rejected.status_code == 422 and '3 个输出张量' in rejected.json()['detail']
    assert manager.current() is None
    options['models'][0]['model_type'] = 'yolov8_det'
    created = client.post(BASE + '/jobs', data={'settings': json.dumps(options)}, files={'files': ('x.jpg', b'ok')})
    assert created.status_code == 202, created.text
    assert wait(manager, created.json()['id'])['status'] == 'completed'
    assert (catalog.runtime / 'probes.log').read_text().splitlines() == ['model.rknn']

    # A file replaced after UI validation is probed again at submission.
    Path(path).write_bytes(b'broken model replacement')
    rejected = client.post(BASE + '/jobs', data={'settings': json.dumps(options)}, files={'files': ('x.jpg', b'ok')})
    assert rejected.status_code == 422 and '无法加载' in rejected.json()['detail']
    assert manager.current()['id'] == created.json()['id']
    changed = client.get(BASE + '/models/info', params=query).json()
    assert not changed['ok'] and changed['file_version'] != checked.json()['file_version']
    assert len((catalog.runtime / 'probes.log').read_text().splitlines()) == 2


@pytest.mark.parametrize('change', ['relative', 'missing', 'wrong_suffix', 'unknown_type'])
def test_model_inspection_rejects_invalid_selection_before_probe(console, change):
    client, _, catalog = console
    query = dict(path=settings(catalog)['models'][0]['model_path'], model_type='yolov8_det')
    if change == 'relative': query['path'] = 'model.rknn'
    if change == 'missing': query['path'] += '.missing.rknn'
    if change == 'wrong_suffix': query['path'] = str(catalog.runtime.parent / 'models/labels.txt')
    if change == 'unknown_type': query['model_type'] = 'unknown'
    assert client.get(BASE + '/models/info', params=query).status_code == 422
    assert not (catalog.runtime / 'probes.log').exists()


def test_current_batch_replaces_history_and_delete_endpoints(console):
    client, manager, _ = console
    assert client.get(BASE + '/jobs/current').json() is None
    paths = client.get('/openapi.json').json()['paths']
    assert 'get' not in paths[BASE + '/jobs']
    assert BASE + '/jobs/{job_id}' not in paths
    payload = {'settings': json.dumps(settings(manager.catalog))}
    first = client.post(BASE + '/jobs', data=payload, files={'files': ('first.jpg', b'ok')}).json()
    wait(manager, first['id'])
    assert client.get(BASE + '/jobs/current').json()['id'] == first['id']
    second = client.post(BASE + '/jobs', data=payload, files={'files': ('slow.jpg', b'ok')}).json()
    current = client.get(BASE + '/jobs/current').json()
    assert isinstance(current, dict) and current['id'] == second['id']
    assert current['status'] not in jobs.TERMINAL
    assert client.get(BASE + '/jobs').status_code == 405
    assert client.get(f"{BASE}/jobs/{first['id']}").status_code == 404
    assert client.delete(f"{BASE}/jobs/{first['id']}").status_code == 404
    assert client.post(f"{BASE}/jobs/{second['id']}/cancel").status_code == 200
    wait(manager, second['id'])
    assert client.get(BASE + '/jobs/current').json()['status'] == 'cancelled'
    assert client.get(f"{BASE}/jobs/{first['id']}/items/0").status_code == 200


def test_result_cache_prunes_private_copies_and_preserves_source(console, tmp_path, monkeypatch):
    client, manager, catalog = console
    monkeypatch.setattr(jobs, 'RESULT_CACHE_LIMIT', 1)
    source = tmp_path / 'source.jpg'
    source.write_bytes(b'ok')
    payload = dict(path=str(source), settings=settings(catalog))
    first = client.post(BASE + '/jobs/from-device', json=payload).json()
    wait(manager, first['id'])
    second = client.post(BASE + '/jobs/from-device', json=payload).json()
    wait(manager, second['id'])
    manager.shutdown()
    assert not (manager.root / first['id']).exists()
    assert (manager.root / second['id']).is_dir() and source.read_bytes() == b'ok'
    recovered = jobs.ImageJobs(manager.root, catalog)
    assert recovered.current()['id'] == second['id']
    assert recovered.result(second['id'], 0)['results'] == []
    for index in [-1, 1]:
        assert client.get(f"{BASE}/jobs/{second['id']}/items/{index}").status_code == 404


def test_clear_finished_batches_removes_cache_and_preserves_models_and_source(console, tmp_path, monkeypatch):
    client, manager, catalog = console
    source = tmp_path / 'source.jpg'
    source.write_bytes(b'ok')
    uploaded = client.post(BASE + '/assets/upload', files={'file': ('saved.rknn', b'model')}).json()
    options = settings(catalog)
    for model in options['models']:
        model['model_path'] = uploaded['path']
    first = client.post(BASE + '/jobs/from-device', json=dict(path=str(source), settings=options)).json()
    assert wait(manager, first['id'])['status'] == 'completed'
    assert client.get(f"{BASE}/jobs/{first['id']}/download").status_code == 200
    second = client.post(BASE + '/jobs', data={'settings': json.dumps(options)},
                         files={'files': ('bad.jpg', b'bad')}).json()
    assert wait(manager, second['id'])['status'] == 'completed_with_errors'
    cancelled = manager.reserve(options, ['cancelled.jpg'])
    assert client.post(f'{BASE}/jobs/{cancelled}/cancel').json()['status'] == 'cancelled'
    failed = manager.reserve(options, ['failed.jpg'])
    manager.input_path(failed, 0).write_bytes(b'ok')
    with monkeypatch.context() as patch:
        patch.setattr(catalog, 'binary', tmp_path / 'missing-runner')
        manager.submit(failed)
        assert wait(manager, failed)['status'] == 'failed'

    before = list(manager.root.iterdir())
    assert len(before) == 4 and (manager.root / first['id'] / 'results.zip').is_file()
    response = client.delete(BASE + '/jobs/completed')
    assert response.status_code == 200, response.text
    assert response.json() == dict(cleared=4, current=None)
    assert all(not path.exists() for path in before)
    assert source.read_bytes() == b'ok' and Path(uploaded['path']).read_bytes() == b'model'
    assert Path(options['models'][0]['label_path']).read_text() == 'person\n'
    assert uploaded['path'] in client.get(BASE + '/assets').json()['models']
    assert client.get(BASE + '/jobs/current').json() is None
    assert jobs.ImageJobs(manager.root, catalog).current() is None
    assert client.get(f"{BASE}/jobs/{first['id']}/items/0").status_code == 404
    assert client.get(f"{BASE}/jobs/{first['id']}/download").status_code == 404
    assert client.delete(BASE + '/jobs/completed').json() == dict(cleared=0, current=None)
    next_job = client.post(BASE + '/jobs/from-device', json=dict(path=str(source), settings=options)).json()
    assert wait(manager, next_job['id'])['status'] == 'completed'


@pytest.mark.parametrize('active_status', ['uploading', 'running'])
def test_clear_records_preserves_active_batch(console, active_status):
    client, manager, catalog = console
    options = settings(catalog)
    completed = client.post(BASE + '/jobs', data={'settings': json.dumps(options)},
                            files={'files': ('first.jpg', b'ok')}).json()
    wait(manager, completed['id'])
    active_id = manager.reserve(options, ['slow.jpg' if active_status == 'running' else 'next.jpg'])
    source = manager.input_path(active_id, 0)
    source.write_bytes(b'ok')
    if active_status == 'running':
        manager.submit(active_id)
        deadline = time.monotonic() + 5
        while manager.current()['status'] != 'running' and time.monotonic() < deadline:
            time.sleep(.03)
    assert manager.current()['status'] == active_status
    response = client.delete(BASE + '/jobs/completed')
    assert response.status_code == 200, response.text
    assert response.json()['cleared'] == 1
    assert response.json()['current']['id'] == active_id
    assert response.json()['current']['status'] == active_status
    assert source.read_bytes() == b'ok' and not (manager.root / completed['id']).exists()
    assert client.delete(BASE + '/jobs/completed').json()['cleared'] == 0
    if active_status == 'uploading':
        manager.submit(active_id)
        assert wait(manager, active_id)['status'] == 'completed'
    else:
        assert client.post(f'{BASE}/jobs/{active_id}/cancel').status_code == 200
        assert wait(manager, active_id)['status'] == 'cancelled'


def test_clear_storage_failure_is_reported_and_can_be_retried(console, monkeypatch):
    client, manager, catalog = console
    job = client.post(BASE + '/jobs', data={'settings': json.dumps(settings(catalog))},
                      files={'files': ('first.jpg', b'ok')}).json()
    wait(manager, job['id'])
    with monkeypatch.context() as patch:
        def denied(path):
            raise PermissionError(errno.EACCES, 'Permission denied', str(path))
        patch.setattr(jobs.shutil, 'rmtree', denied)
        response = client.delete(BASE + '/jobs/completed')
        assert response.status_code == 503
        assert '存储不可用' in response.json()['detail']
    assert client.get(BASE + '/jobs/current').json()['id'] == job['id']
    assert (manager.root / job['id'] / 'status.json').is_file()
    assert client.delete(BASE + '/jobs/completed').json() == dict(cleared=1, current=None)


def test_cached_export_drops_json_and_preserves_result_files(console):
    client, manager, _ = console
    response = client.post(BASE + '/jobs', data={'settings': json.dumps(settings(manager.catalog))},
                           files={'files': ('x.jpg', b'ok')})
    assert response.status_code == 202
    job_id = response.json()['id']
    assert wait(manager, job_id)['status'] == 'completed'
    directory = manager.root / job_id
    output = directory / 'output/000000'
    (output / 'model_0.jpg').write_bytes(b'model image')
    (output / 'model_0_mask.png').write_bytes(b'mask')
    (output / 'model_0_depth.npy').write_bytes(b'depth')
    target = directory / 'results.zip'
    with zipfile.ZipFile(target, 'w') as archive:
        archive.writestr('task.json', '{}')
        archive.writestr('000000/result.json', '{}')
        archive.writestr('000000/annotated.jpg', b'old jpeg')
    response = client.get(f'{BASE}/jobs/{job_id}/download')
    assert response.status_code == 200
    with zipfile.ZipFile(BytesIO(response.content)) as archive:
        assert set(archive.namelist()) == {'000000/original.jpg', '000000/annotated.jpg',
                                         '000000/model_0.jpg', '000000/model_0_mask.png',
                                         '000000/model_0_depth.npy'}
        assert archive.read('000000/annotated.jpg') == b'jpeg'
    # The structured API still drives the preview; only file exports omit JSON.
    assert client.get(f'{BASE}/jobs/{job_id}/items/0').status_code == 200
    modified = target.stat().st_mtime_ns
    assert client.get(f'{BASE}/jobs/{job_id}/download').content == response.content
    assert target.stat().st_mtime_ns == modified


@pytest.mark.parametrize('change', ['duplicate', 'nan', 'outside', 'empty', 'unknown', 'bool', 'roi'])
def test_invalid_configuration_never_reserves_job(console, change, tmp_path):
    client, manager, catalog = console
    value = settings(manager.catalog)
    if change == 'duplicate': value['models'][1]['id'] = 'person'
    if change == 'nan': value['models'][0]['obj_thresh'] = float('nan')
    if change == 'outside': value['models'][0]['model_path'] = '../model.rknn'
    if change == 'unknown': value['models'][0]['model_type'] = 'unknown'
    if change == 'bool': value['models'][0]['obj_thresh'] = True
    if change == 'roi': value['inference_roi'] = dict(mode='roi_only', polygon=[[0, 0]])
    if change == 'empty':
        empty = tmp_path / 'empty.rknn'; empty.touch()
        value['models'][0]['model_path'] = str(empty)
    response = client.post(BASE + '/jobs', data={'settings': json.dumps(value)}, files={'files': ('x.jpg', b'ok')})
    assert response.status_code == 422, response.text
    assert manager.current() is None


def test_cancel_busy_reject_and_reuse(console):
    client, manager, _ = console
    payload = {'settings': json.dumps(settings(manager.catalog))}
    response = client.post(BASE + '/jobs', data=payload, files={'files': ('slow.jpg', b'ok')})
    job_id = response.json()['id']
    assert client.post(BASE + '/jobs', data=payload, files={'files': ('x.jpg', b'ok')}).status_code == 409
    assert client.post(f'{BASE}/jobs/{job_id}/cancel').status_code == 200
    assert wait(manager, job_id)['status'] == 'cancelled'
    again = client.post(BASE + '/jobs', data=payload, files={'files': ('next.jpg', b'ok')})
    assert wait(manager, again.json()['id'])['status'] == 'completed'


def test_upload_failure_releases_slot_and_restart_interrupts_upload(console):
    client, manager, catalog = console
    response = client.post(BASE + '/jobs', data={'settings': json.dumps(settings(manager.catalog))}, files={'files': ('x.jpg', b'')})
    assert response.status_code == 422
    assert manager.current() is None
    job_id = manager.reserve(settings(manager.catalog), ['x.jpg'])
    restarted = jobs.ImageJobs(manager.root, catalog)
    assert restarted.current()['status'] == 'failed'
    assert '重启' in restarted.current()['error']


def test_device_lock_is_authoritative_and_not_unlinked(console):
    import fcntl
    client, manager, catalog = console
    with engine.NPU_LOCK_FILE.open('a') as lock:
        inode = engine.NPU_LOCK_FILE.stat().st_ino
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert catalog.capabilities()['busy']
        response = client.post(BASE + '/jobs', data={'settings': json.dumps(settings(manager.catalog))}, files={'files': ('x.jpg', b'ok')})
        assert response.status_code == 409
        assert manager.current() is None
        assert engine.NPU_LOCK_FILE.stat().st_ino == inode
    assert not catalog.capabilities()['busy']


def test_old_engine_is_never_launched_for_capability_discovery(console):
    _, manager, catalog = console
    binary = catalog.binary
    marker = binary.parent / 'unexpected-start'
    binary.write_text(f'#!/bin/sh\ntouch {marker}\n')
    with pytest.raises(engine.JobError, match='不可用'):
        catalog.capabilities()
    assert not marker.exists()
    assert manager.current() is None


def test_relative_storage_root_works_in_native_job_working_directory(console, tmp_path, monkeypatch):
    _, _, catalog = console
    monkeypatch.chdir(tmp_path)
    manager = jobs.ImageJobs(Path('relative-jobs'), catalog)
    try:
        job_id = manager.reserve(settings(manager.catalog), ['x.jpg'])
        manager.input_path(job_id, 0).write_bytes(b'ok')
        manager.submit(job_id)
        assert wait(manager, job_id)['status'] == 'completed'
        assert manager.result(job_id, 0)['results'] == []
    finally:
        manager.shutdown()


def test_device_directory_uses_same_batch_and_preserves_source(console, tmp_path):
    client, manager, catalog = console
    folder = tmp_path / '板端图片'
    nested = folder / '子文件夹'
    nested.mkdir(parents=True)
    files = {folder / 'a.JPG': b'ok', folder / 'b.png': b'bad', nested / 'a.JPG': b'ok'}
    for path, value in files.items():
        path.write_bytes(value)
    (folder / 'notes.txt').write_text('not an image')
    (nested / 'loop').symlink_to(folder, target_is_directory=True)
    payload = dict(path=str(folder), recursive=False)
    preview = client.post(BASE + '/device-images', json=payload)
    assert preview.status_code == 200, preview.text
    assert [i['name'] for i in preview.json()['images']] == ['a.JPG', 'b.png']
    payload['recursive'] = True
    preview = client.post(BASE + '/device-images', json=payload).json()
    assert [i['name'] for i in preview['images']] == ['a.JPG', 'b.png', '子文件夹/a.JPG']
    assert preview['total_bytes'] == 7
    response = client.post(BASE + '/jobs/from-device', json=dict(payload, settings=settings(manager.catalog)))
    assert response.status_code == 202, response.text
    job_id = response.json()['id']
    result = wait(manager, job_id)
    assert result['status'] == 'completed_with_errors'
    assert [i['status'] for i in result['items']] == ['success', 'failed', 'success']
    assert [i['name'] for i in result['items']] == [i['name'] for i in preview['images']]
    assert [i['source_path'] for i in result['items']] == [i['path'] for i in preview['images']]
    assert result['source'] == dict(kind='device', path=str(folder), is_directory=True, recursive=True)
    details = client.get(f'{BASE}/jobs/{job_id}/items/2').json()
    assert [m['obj_thresh'] for m in details['models']] == [.9, .2]
    recovered = jobs.ImageJobs(manager.root, catalog).current()
    assert recovered['source'] == result['source'] and recovered['items'] == result['items']
    assert all(path.read_bytes() == value for path, value in files.items())


def test_device_single_symlink_keeps_name_and_copies_independent_input(console, tmp_path):
    client, manager, _ = console
    original = tmp_path / 'no-extension'
    original.write_bytes(b'ok')
    alias = tmp_path / '测试图.jpg'
    alias.symlink_to(original)
    response = client.post(BASE + '/jobs/from-device', json=dict(path=str(alias), settings=settings(manager.catalog)))
    assert response.status_code == 202, response.text
    job_id = response.json()['id']
    result = wait(manager, job_id)
    assert result['status'] == 'completed'
    assert result['source']['path'] == str(alias) and not result['source']['is_directory']
    assert result['items'][0]['name'] == alias.name
    assert result['items'][0]['source_path'] == str(original)
    snapshot = manager.root / job_id / 'input/000000.jpg'
    assert snapshot.stat().st_ino != original.stat().st_ino
    original.write_bytes(b'changed')
    assert snapshot.read_bytes() == b'ok'
    assert original.read_bytes() == b'changed' and alias.is_symlink()


@pytest.mark.parametrize('kind', ['relative', 'missing', 'empty_folder', 'empty_file', 'extension', 'fifo', 'bad_recursive'])
def test_invalid_device_source_does_not_reserve(console, tmp_path, kind):
    import os
    client, manager, _ = console
    path = tmp_path / 'input.jpg'
    if kind == 'relative': path = Path('input.jpg')
    if kind == 'empty_folder': path.mkdir()
    if kind == 'empty_file': path.touch()
    if kind == 'extension':
        path = path.with_suffix('.txt'); path.write_text('not an image')
    if kind == 'fifo': os.mkfifo(path)
    if kind == 'bad_recursive': path.write_bytes(b'ok')
    payload = dict(path=str(path), recursive='true' if kind == 'bad_recursive' else False)
    assert client.post(BASE + '/device-images', json=payload).status_code == 422
    response = client.post(BASE + '/jobs/from-device', json=dict(payload, settings=settings(manager.catalog)))
    assert response.status_code == 422, response.text
    assert manager.current() is None


@pytest.mark.parametrize('limit', ['MAX_IMAGES', 'MAX_IMAGE_BYTES', 'MAX_UPLOAD_BYTES', 'MAX_SCAN_ENTRIES'])
def test_device_limits_reject_instead_of_truncating(console, tmp_path, monkeypatch, limit):
    client, manager, _ = console
    folder = tmp_path / 'images'; folder.mkdir()
    for name in ['one.jpg', 'two.png']:
        (folder / name).write_bytes(b'ok')
    monkeypatch.setattr(sources, limit, 1)
    response = client.post(BASE + '/jobs/from-device', json=dict(path=str(folder), settings=settings(manager.catalog)))
    assert response.status_code == 422, response.text
    assert manager.current() is None


def test_changed_device_file_rejected_and_slot_released(console, tmp_path, monkeypatch):
    client, manager, _ = console
    path = tmp_path / 'input.jpg'; path.write_bytes(b'ok')
    snapshot = jobs.snapshot_device_image
    def mutate_then_copy(image, target, checkpoint):
        path.write_bytes(b'changed')
        snapshot(image, target, checkpoint)
    monkeypatch.setattr(jobs, 'snapshot_device_image', mutate_then_copy)
    payload = dict(path=str(path), settings=settings(manager.catalog))
    response = client.post(BASE + '/jobs/from-device', json=payload)
    assert response.status_code == 409, response.text
    assert manager.current() is None
    assert list(manager.root.iterdir()) == []
    monkeypatch.setattr(jobs, 'snapshot_device_image', snapshot)
    response = client.post(BASE + '/jobs/from-device', json=payload)
    assert response.status_code == 202, response.text
    assert wait(manager, response.json()['id'])['status'] == 'completed'


def test_empty_app_catalog_can_upload_models_and_infer(console, tmp_path):
    client, manager, _ = console
    assert not (tmp_path / 'apps').exists()
    assert client.get(BASE + '/assets').json() == dict(models=[], labels=[])
    assert client.get(BASE + '/capabilities').status_code == 200
    model = client.post(BASE + '/assets/upload', files={'file': ('test.rknn', b'model')}).json()
    label = client.post(BASE + '/assets/upload', files={'file': ('test.txt', b'person\n')}).json()
    repeat = client.post(BASE + '/assets/upload', files={'file': ('test.rknn', b'other')}).json()
    assert model['path'] != repeat['path'] and repeat['renamed']
    assert Path(model['path']).read_bytes() == b'model'
    value = settings(manager.catalog)
    for item in value['models']:
        item.update(model_path=model['path'], label_path=label['path'])
    response = client.post(BASE + '/jobs', data={'settings': json.dumps(value)}, files={'files': ('x.jpg', b'ok')})
    assert response.status_code == 202, response.text
    result = wait(manager, response.json()['id'])
    assert result['status'] == 'completed' and 'app' not in result
    assert all(m['model_path'] == model['path'] for m in result['settings']['models'])
    assert not (tmp_path / 'apps').exists()


@pytest.mark.parametrize('name,content', [('empty.rknn', b''), ('program.sh', b'script')])
def test_bad_model_import_leaves_library_clean(console, name, content):
    client, _, _ = console
    response = client.post(BASE + '/assets/upload', files={'file': (name, content)})
    assert response.status_code == 422, response.text
    assert client.get(BASE + '/assets').json() == dict(models=[], labels=[])
    assert not routes.model_assets.root.exists() or list(routes.model_assets.root.iterdir()) == []


def test_old_tasks_remain_viewable_without_the_original_program(console):
    client, manager, catalog = console
    job_id = manager.reserve(settings(catalog), ['x.jpg'])
    manager.input_path(job_id, 0).write_bytes(b'ok')
    manager.submit(job_id)
    wait(manager, job_id)
    path = manager.root / job_id / 'status.json'
    record = jobs.read_json(path)
    record['app'] = 'removed-program'
    jobs.write_json(path, record)
    restored = jobs.ImageJobs(manager.root, catalog)
    assert restored.current()['id'] == job_id
    assert restored.result(job_id, 0)['results'] == []
    assert restored.artifact(job_id, '000000/annotated.jpg').is_file()


def test_canvas_preset_only_resolves_paths_and_needs_no_program_binary(console, tmp_path):
    client, manager, _ = console
    assets = tmp_path / 'apps/canvas-demo/assets'
    assets.mkdir(parents=True)
    (assets / 'model.rknn').write_bytes(b'model')
    (assets / 'labels.txt').write_text('person\n')
    value = settings(manager.catalog)
    for model in value['models']:
        model.update(model_path='assets/model.rknn', label_path='assets/labels.txt')
    response = client.post(BASE + '/canvas-preset', json=dict(app='canvas-demo', settings=value))
    assert response.status_code == 200, response.text
    assert response.json()['models'][0]['model_path'] == str(assets / 'model.rknn')
    assert response.json()['models'][1]['obj_thresh'] == .2
    assert value['models'][0]['model_path'] == 'assets/model.rknn'
    value['models'][0]['model_path'] = '../other.rknn'
    assert client.post(BASE + '/canvas-preset', json=dict(app='canvas-demo', settings=value)).status_code == 422


@pytest.mark.parametrize('suffix,limit', [('.rknn', 'MAX_MODEL_BYTES'), ('.txt', 'MAX_LABEL_BYTES')])
def test_model_import_size_limit_removes_partial_file(console, monkeypatch, suffix, limit):
    from services.image_inference import assets
    client, _, _ = console
    monkeypatch.setattr(assets, limit, 1)
    response = client.post(BASE + '/assets/upload', files={'file': ('large' + suffix, b'too big')})
    assert response.status_code == 413
    assert list(routes.model_assets.root.iterdir()) == []
