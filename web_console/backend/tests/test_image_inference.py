from io import BytesIO
import errno
import json
from pathlib import Path
import time
import zipfile

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from services.image_inference import engine, jobs, sources, storage
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
out = pathlib.Path(job['output_dir']); out.mkdir(exist_ok=True)
items = [dict(index=i,name=x['name'],status='pending') for i,x in enumerate(job['images'])]
if job.get('resume'): items=json.loads((out/'progress.json').read_text())['items']
def progress(status):
 p=out/'progress.json.tmp';p.write_text(json.dumps(dict(status=status,items=items)));p.replace(out/'progress.json')
def cancel(*args):
 progress('cancelled');sys.exit(130)
signal.signal(signal.SIGINT, cancel)
progress('running')
if not job.get('resume') and any(x['name'].startswith('slow') for x in job['images']): time.sleep(30)
failures=0
for i, image in enumerate(job['images']):
 if items[i]['status'] in ('success', 'partial', 'failed'):
  failures+=items[i]['status']!='success';continue
 if image['name'].startswith('wait') and not job.get('resume'): time.sleep(30)
 with (out/'executed.log').open('a') as log: print(i, file=log)
 if pathlib.Path(image['path']).read_bytes()==b'bad':
  items[i].update(status='failed',error='Cannot decode image');failures+=1
 else:
  directory=out/f'{i:06d}';directory.mkdir(exist_ok=True)
  (directory/'result.json').write_text(json.dumps(dict(coordinate_width=640,coordinate_height=640,source_width=1280,source_height=720,models=[dict(m,type=m['model_type'],succeeded=True) for m in job['models']],results=[])))
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


def upload_batch(client, data, files):
    uploads = [files['files']] if isinstance(files, dict) else [item[1] for item in files]
    response = client.post(BASE + '/jobs', content=json.dumps({
        'settings': json.loads(data['settings']),
        'files': [dict(name=item[0], size=len(item[1])) for item in uploads],
    }), headers={'Content-Type': 'application/json'})
    if response.status_code != 202:
        return response
    job_id = response.json()['id']
    for index, item in enumerate(uploads):
        response = client.put(f'{BASE}/jobs/{job_id}/inputs/{index}', content=item[1])
        if response.status_code != 200:
            return response
    return client.post(f'{BASE}/jobs/{job_id}/submit')


def wait(manager, job_id):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        result = manager.current()
        assert result is not None and result['id'] == job_id
        if result['status'] in jobs.TERMINAL or result['status'] == 'paused_storage':
            return result
        time.sleep(.03)
    raise AssertionError(manager.current())


def test_batch_multi_model_partial_failure_and_restart(console):
    client, manager, catalog = console
    response = upload_batch(client, data={'settings': json.dumps(settings(manager.catalog))}, files=[
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
    assert client.get(f'{BASE}/jobs/{job_id}/download').status_code == 404
    assert client.post(f'{BASE}/jobs/{job_id}/labels', json={'indices': [0, 2]}).status_code == 422
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
    rejected = upload_batch(client, data={'settings': json.dumps(options)}, files={'files': ('x.jpg', b'ok')})
    assert rejected.status_code == 422 and '3 个输出张量' in rejected.json()['detail']
    assert manager.current() is None
    options['models'][0]['model_type'] = 'yolov8_det'
    created = upload_batch(client, data={'settings': json.dumps(options)}, files={'files': ('x.jpg', b'ok')})
    assert created.status_code == 202, created.text
    assert wait(manager, created.json()['id'])['status'] == 'completed'
    assert (catalog.runtime / 'probes.log').read_text().splitlines() == ['model.rknn']

    # A file replaced after UI validation is probed again at submission.
    Path(path).write_bytes(b'broken model replacement')
    rejected = upload_batch(client, data={'settings': json.dumps(options)}, files={'files': ('x.jpg', b'ok')})
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
    first = upload_batch(client, data=payload, files={'files': ('first.jpg', b'ok')}).json()
    wait(manager, first['id'])
    assert client.get(BASE + '/jobs/current').json()['id'] == first['id']
    second = upload_batch(client, data=payload, files={'files': ('slow.jpg', b'ok')}).json()
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
    options = settings(catalog)
    first = client.post(BASE + '/jobs/from-device', json=dict(path=str(source), settings=options)).json()
    assert wait(manager, first['id'])['status'] == 'completed'
    # Old cached bundles must also disappear when records are cleared.
    (manager.root / first['id'] / 'results.zip').write_bytes(b'old cache')
    second = upload_batch(client, data={'settings': json.dumps(options)},
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
    assert source.read_bytes() == b'ok' and Path(options['models'][0]['model_path']).read_bytes() == b'model'
    assert Path(options['models'][0]['label_path']).read_text() == 'person\n'
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
    completed = upload_batch(client, data={'settings': json.dumps(options)},
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
    job = upload_batch(client, data={'settings': json.dumps(settings(catalog))},
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
    response = upload_batch(client, data={'settings': json.dumps(value)}, files={'files': ('x.jpg', b'ok')})
    assert response.status_code == 422, response.text
    assert manager.current() is None


def test_cancel_busy_reject_and_reuse(console):
    client, manager, _ = console
    payload = {'settings': json.dumps(settings(manager.catalog))}
    response = upload_batch(client, data=payload, files={'files': ('slow.jpg', b'ok')})
    job_id = response.json()['id']
    assert upload_batch(client, data=payload, files={'files': ('x.jpg', b'ok')}).status_code == 409
    assert client.post(f'{BASE}/jobs/{job_id}/cancel').status_code == 200
    assert wait(manager, job_id)['status'] == 'cancelled'
    again = upload_batch(client, data=payload, files={'files': ('next.jpg', b'ok')})
    assert wait(manager, again.json()['id'])['status'] == 'completed'


def test_upload_failure_releases_slot_and_restart_interrupts_upload(console):
    client, manager, catalog = console
    response = upload_batch(client, data={'settings': json.dumps(settings(manager.catalog))}, files={'files': ('x.jpg', b'')})
    assert response.status_code == 422
    assert manager.current() is None
    job_id = manager.reserve(settings(manager.catalog), ['x.jpg'])
    restarted = jobs.ImageJobs(manager.root, catalog)
    assert restarted.current()['status'] == 'paused_storage'
    assert '重启' in restarted.current()['error']


def test_device_lock_is_authoritative_and_not_unlinked(console):
    import fcntl
    client, manager, catalog = console
    with engine.NPU_LOCK_FILE.open('a') as lock:
        inode = engine.NPU_LOCK_FILE.stat().st_ino
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert catalog.capabilities()['busy']
        response = upload_batch(client, data={'settings': json.dumps(settings(manager.catalog))}, files={'files': ('x.jpg', b'ok')})
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


def test_browse_device_folders(console, tmp_path):
    client, _, _ = console
    folder = tmp_path / '测试图片'
    for name in ('第 1 组', 'B', 'a'):
        (folder / name).mkdir(parents=True)
    image = folder / 'photo.png'
    image.touch()
    (folder / 'loop').symlink_to(folder, target_is_directory=True)
    url = BASE + '/device-folders'
    listing = dict(path=str(folder), parent=str(tmp_path), folders=['a', 'B', '第 1 组'])
    for path in (folder, image):
        assert client.get(url, params={'path': str(path)}).json() == listing
    assert client.get(url, params={'path': str(folder / 'a')}).json()['folders'] == []
    assert client.get(url).json()['parent'] is None
    for path in ('relative', str(folder / 'missing')):
        assert client.get(url, params={'path': path}).status_code == 422


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


def test_device_upload_budget_rejects_instead_of_truncating(console, tmp_path, monkeypatch):
    client, manager, catalog = console
    folder = tmp_path / 'pictures'
    folder.mkdir()
    (folder / 'one.jpg').write_bytes(b'ok')
    (folder / 'two.jpg').write_bytes(b'ok')
    from collections import namedtuple
    Usage = namedtuple('Usage', 'total used free')
    monkeypatch.setattr(storage.shutil, 'disk_usage', lambda _: Usage(0, 0, storage.MIN_FREE_BYTES + 4))
    response = client.post(BASE + '/jobs/from-device', json=dict(path=str(folder), settings=settings(catalog)))
    assert response.status_code == 507
    assert manager.current() is None
    assert (folder / 'one.jpg').read_bytes() == b'ok'


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


def test_standalone_assets_survive_job_cleanup_and_duplicate_import(console, tmp_path):
    client, manager, _ = console
    folder = tmp_path / 'apps/_console/model_assets'
    folder.mkdir(parents=True)
    original = folder / 'saved.rknn'
    original.write_bytes(b'user model')
    upload = BASE + '/assets/upload'
    assert client.post(upload, files={'file': ('empty.rknn', b'')}).status_code == 422
    assert client.post(upload, files={'file': ('script.py', b'x')}).status_code == 422
    result = client.post(upload, files={'file': ('saved.rknn', b'new model')}).json()
    assert result == {'path': str(folder / 'saved_copy.rknn'), 'renamed': True}
    label = client.post(upload, files={'file': ('person.txt', b'person')}).json()['path']
    params = settings(manager.catalog)
    for model in params['models']:
        model.update(model_path=result['path'], label_path=label)
    response = upload_batch(client, data={'settings': json.dumps(params)}, files={'files': ('x.jpg', b'ok')})
    assert response.status_code == 202, response.text
    assert wait(manager, response.json()['id'])['status'] == 'completed'
    assert client.delete(BASE + '/jobs/completed').json()['cleared'] == 1
    assert client.get(BASE + '/assets').json() == {'models': [str(original), result['path']], 'labels': [label]}
    assert original.read_bytes() == b'user model'


def test_package_models_infer_without_copying_to_shared_library(console, tmp_path):
    client, manager, _ = console
    assets = tmp_path / 'apps/demo/assets'
    assets.mkdir(parents=True)
    model, label = assets / 'test.rknn', assets / 'test.txt'
    model.write_bytes(b'model')
    label.write_text('person\n')
    value = settings(manager.catalog)
    for item in value['models']:
        item.update(model_path=str(model), label_path=str(label))
    response = upload_batch(client, data={'settings': json.dumps(value)}, files={'files': ('x.jpg', b'ok')})
    assert response.status_code == 202, response.text
    result = wait(manager, response.json()['id'])
    assert result['status'] == 'completed'
    assert all(m['model_path'] == str(model) for m in result['settings']['models'])
    assert model.read_bytes() == b'model' and label.read_text() == 'person\n'
    assert not (tmp_path / 'apps/_console/model_assets').exists()


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


def test_removed_canvas_adapter_is_not_routed(console):
    client, _, _ = console
    assert client.post(BASE + '/canvas-preset', json={}).status_code == 404


def label_batch(console, names=('first.jpg', 'second.png', 'empty.jpg', 'bad.jpg')):
    client, manager, catalog = console
    options = settings(catalog)
    options['models'] = options['models'][:1]
    response = upload_batch(client, data={'settings': json.dumps(options)},
                           files=[('files', (name, b'bad' if name == 'bad.jpg' else b'ok')) for name in names])
    assert response.status_code == 202, response.text
    job_id = response.json()['id']
    wait(manager, job_id)
    result = manager.result(job_id, 0)
    result['results'] = [dict(model_id='person', model_type='yolov8_det', class_id=3,
                              box=dict(x=160, y=80, width=320, height=160))]
    jobs.write_json(manager.root / job_id / 'output/000000/result.json', result)
    return job_id, result


def test_selected_yolo_labels_only_and_empty_detection(console):
    client, manager, _ = console
    job_id, _ = label_batch(console)
    response = client.post(f'{BASE}/jobs/{job_id}/labels', json={'indices': [2, 0]})
    assert response.status_code == 200, response.text
    assert response.headers['content-type'] == 'application/zip'
    with zipfile.ZipFile(BytesIO(response.content)) as archive:
        assert archive.namelist() == ['first.txt', 'empty.txt']
        assert archive.read('first.txt').decode() == '3 0.50000000 0.25000000 0.50000000 0.25000000\n'
        assert archive.read('empty.txt') == b''
    # Every successful image can be selected, and subsets do not reuse a cached ZIP.
    response = client.post(f'{BASE}/jobs/{job_id}/labels', json={'indices': [0, 1, 2]})
    with zipfile.ZipFile(BytesIO(response.content)) as archive:
        assert archive.namelist() == ['first.txt', 'second.txt', 'empty.txt']
    assert not (manager.root / job_id / 'results.zip').exists()
    assert client.get(f'{BASE}/jobs/{job_id}/download').status_code == 404
    assert client.get(f'{BASE}/jobs/{job_id}/files/000000/original.jpg').status_code == 200
    for name in ('result.json', 'model_0_mask.png', 'model_0_depth.npy'):
        assert client.get(f'{BASE}/jobs/{job_id}/files/000000/{name}').status_code == 404


@pytest.mark.parametrize('indices', [[], [0, 0], [-1], [4], [3], [True], ['0'], [0.5], list(range(101))])
def test_label_selection_rejects_invalid_or_failed_images(console, indices):
    client, _, _ = console
    job_id, _ = label_batch(console)
    assert client.post(f'{BASE}/jobs/{job_id}/labels', json={'indices': indices}).status_code == 422


def test_label_names_do_not_overwrite_or_escape_archive(console):
    client, _, _ = console
    job_id, _ = label_batch(console, ('same.jpg', 'same.png', '中文.jpg', '../escape.jpg'))
    response = client.post(f'{BASE}/jobs/{job_id}/labels', json={'indices': [0, 1, 2, 3]})
    with zipfile.ZipFile(BytesIO(response.content)) as archive:
        assert archive.namelist() == ['000001/same.txt', '000002/same.txt', '中文.txt', 'escape.txt']
    response = client.post(f'{BASE}/jobs/{job_id}/labels', json={'indices': [1]})
    with zipfile.ZipFile(BytesIO(response.content)) as archive:
        assert archive.namelist() == ['000002/same.txt']


@pytest.mark.parametrize('model_type', ['yolov5', 'yolov8_det', 'yolov8_obb', 'yolov8_pose', 'yolo26_pose', 'yolov5_seg', 'zip_depth'])
def test_export_checks_recorded_model_type(console, model_type):
    client, manager, _ = console
    job_id, result = label_batch(console)
    manager._jobs[job_id].data['settings']['models'][0]['model_type'] = model_type
    result['models'][0]['type'] = model_type
    result['results'][0]['model_type'] = model_type
    jobs.write_json(manager.root / job_id / 'output/000000/result.json', result)
    response = client.post(f'{BASE}/jobs/{job_id}/labels', json={'indices': [0]})
    assert response.status_code == (200 if model_type in ('yolov5', 'yolov8_det') else 422)


def test_label_boxes_use_coordinate_size_and_clip_to_image(console):
    client, manager, _ = console
    job_id, result = label_batch(console)
    result.update(coordinate_width=320, coordinate_height=160)
    result['results'][0]['box'] = dict(x=-10, y=40, width=170, height=160)
    jobs.write_json(manager.root / job_id / 'output/000000/result.json', result)
    response = client.post(f'{BASE}/jobs/{job_id}/labels', json={'indices': [0]})
    with zipfile.ZipFile(BytesIO(response.content)) as archive:
        assert archive.read('first.txt').decode() == '3 0.25000000 0.62500000 0.50000000 0.75000000\n'


@pytest.mark.parametrize('fault', ['dimensions', 'nan', 'negative_class', 'wrong_model', 'failed_model', 'missing_box'])
def test_label_export_rejects_incomplete_results(console, fault):
    client, manager, _ = console
    job_id, result = label_batch(console)
    if fault == 'dimensions': result['coordinate_width'] = 0
    if fault == 'nan': result['results'][0]['box']['x'] = float('nan')
    if fault == 'negative_class': result['results'][0]['class_id'] = -1
    if fault == 'wrong_model': result['results'][0]['model_id'] = 'other'
    if fault == 'failed_model': result['models'][0]['succeeded'] = False
    if fault == 'missing_box': result['results'][0].pop('box')
    jobs.write_json(manager.root / job_id / 'output/000000/result.json', result)
    assert client.post(f'{BASE}/jobs/{job_id}/labels', json={'indices': [0]}).status_code == 422


def test_label_export_waits_for_batch_and_survives_restart(console):
    client, manager, catalog = console
    job_id, _ = label_batch(console)
    restored = jobs.ImageJobs(manager.root, catalog)
    assert zipfile.is_zipfile(BytesIO(restored.labels_archive(job_id, [0])))
    manager._jobs[job_id].data['status'] = 'running'
    assert client.post(f'{BASE}/jobs/{job_id}/labels', json={'indices': [0]}).status_code == 409
    manager._jobs[job_id].data['status'] = 'completed'


def reserve_upload(client, catalog, files):
    options = settings(catalog)
    options['models'] = options['models'][:1]
    return client.post(BASE + '/jobs', json={'settings': options, 'files': files})


def test_upload_and_export_more_than_one_thousand_images(console):
    client, manager, catalog = console
    count = 1001
    response = reserve_upload(client, catalog, [dict(name=f'{i}.jpg', size=2) for i in range(count)])
    assert response.status_code == 202, response.text
    job_id = response.json()['id']
    for index in range(count):
        assert client.put(f'{BASE}/jobs/{job_id}/inputs/{index}', content=b'ok').status_code == 200
    assert client.post(f'{BASE}/jobs/{job_id}/submit').status_code == 202
    deadline = time.monotonic() + 30
    while manager.current()['status'] not in jobs.TERMINAL and time.monotonic() < deadline:
        time.sleep(.1)
    assert manager.current()['status'] == 'completed'
    assert manager.current()['completed'] == count
    response = client.post(f'{BASE}/jobs/{job_id}/labels', json={'indices': list(range(count))})
    assert response.status_code == 200, response.text
    with zipfile.ZipFile(BytesIO(response.content)) as archive:
        assert len(archive.namelist()) == count


def test_upload_budget_has_no_fixed_file_or_batch_bytes(console):
    client, manager, catalog = console
    response = reserve_upload(client, catalog, [dict(name='large.jpg', size=300 * 1024**2)])
    assert response.status_code == 202, response.text
    job_id = response.json()['id']
    assert client.delete(f'{BASE}/jobs/{job_id}/upload').status_code == 200
    assert manager.current() is None


@pytest.mark.parametrize('remaining', [-1, 0, 10, 11])
def test_upload_reserves_strictly_more_than_100mb(console, monkeypatch, remaining):
    from collections import namedtuple
    client, manager, catalog = console
    Usage = namedtuple('Usage', 'total used free')
    monkeypatch.setattr(storage.shutil, 'disk_usage', lambda _: Usage(0, 0, storage.MIN_FREE_BYTES + remaining))
    caps = client.get(BASE + '/capabilities').json()
    assert 'max_images' not in caps and 'max_upload_bytes' not in caps
    assert caps['upload_available_bytes'] == max(0, remaining - 1)
    response = reserve_upload(client, catalog, [dict(name='x.jpg', size=10)])
    assert response.status_code == (202 if remaining == 11 else 507)
    if remaining != 11:
        assert manager.current() is None


def test_storage_drop_during_upload_preserves_inputs_and_resumes(console, monkeypatch):
    from collections import namedtuple
    client, manager, catalog = console
    job_id = reserve_upload(client, catalog, [dict(name='first.jpg', size=2), dict(name='second.jpg', size=2)]).json()['id']
    assert client.put(f'{BASE}/jobs/{job_id}/inputs/0', content=b'ok').status_code == 200
    Usage = namedtuple('Usage', 'total used free')
    with monkeypatch.context() as patch:
        patch.setattr(storage.shutil, 'disk_usage', lambda _: Usage(0, 0, storage.MIN_FREE_BYTES + 2))
        response = client.put(f'{BASE}/jobs/{job_id}/inputs/1', content=b'ok')
        assert client.post(f'{BASE}/jobs/{job_id}/resume').status_code == 507
    assert response.status_code == 507 and '存储空间不足' in response.text
    paused = manager.current()
    assert paused['status'] == 'paused_storage' and paused['resume_phase'] == 'upload'
    assert paused['items'][0]['uploaded'] and not paused['items'][1].get('uploaded')
    assert (manager.root / job_id / 'input/000000.jpg').read_bytes() == b'ok'
    assert not (manager.root / job_id / 'input/000001.jpg').exists()
    assert client.delete(BASE + '/jobs/completed').json()['cleared'] == 0
    assert client.post(f'{BASE}/jobs/{job_id}/resume').json()['status'] == 'uploading'
    assert client.put(f'{BASE}/jobs/{job_id}/inputs/1', content=b'ok').status_code == 200
    assert client.post(f'{BASE}/jobs/{job_id}/submit').status_code == 202
    assert wait(manager, job_id)['status'] == 'completed'


@pytest.mark.parametrize('content', [b'', b'o', b'too long'])
def test_incomplete_or_oversized_upload_cleans_up(console, content):
    client, manager, catalog = console
    job_id = reserve_upload(client, catalog, [dict(name='x.jpg', size=2)]).json()['id']
    assert client.put(f'{BASE}/jobs/{job_id}/inputs/0', content=content).status_code == 422
    assert manager.current() is None
    assert not (manager.root / job_id).exists()


def test_uploaded_files_cannot_be_overwritten_and_incomplete_batch_cannot_run(console):
    client, manager, catalog = console
    job_id = reserve_upload(client, catalog, [dict(name='first.jpg', size=2), dict(name='second.jpg', size=2)]).json()['id']
    assert client.put(f'{BASE}/jobs/{job_id}/inputs/0', content=b'ok').status_code == 200
    assert client.put(f'{BASE}/jobs/{job_id}/inputs/0', content=b'xx').status_code == 409
    assert manager.input_path(job_id, 0).read_bytes() == b'ok'
    assert client.put(f'{BASE}/jobs/{job_id}/inputs/9', content=b'ok').status_code == 404
    assert client.post(f'{BASE}/jobs/{job_id}/submit').status_code == 422
    assert client.delete(f'{BASE}/jobs/{job_id}/upload').status_code == 200
    assert manager.current() is None


def test_device_directory_has_no_image_count_limit(console, tmp_path):
    client, manager, catalog = console
    folder = tmp_path / 'many-images'
    folder.mkdir()
    for index in range(101):
        (folder / f'{index}.jpg').write_bytes(b'ok')
    preview = client.post(BASE + '/device-images', json={'path': str(folder)})
    assert preview.json()['total'] == 101
    response = client.post(BASE + '/jobs/from-device', json=dict(path=str(folder), settings=settings(catalog)))
    assert response.status_code == 202, response.text
    assert wait(manager, response.json()['id'])['completed'] == 101


def test_storage_drop_stops_inference(console, monkeypatch):
    from collections import namedtuple
    client, manager, catalog = console
    response = upload_batch(client, data={'settings': json.dumps(settings(catalog))}, files={'files': ('slow.jpg', b'ok')})
    Usage = namedtuple('Usage', 'total used free')
    monkeypatch.setattr(storage.shutil, 'disk_usage', lambda _: Usage(0, 0, storage.MIN_FREE_BYTES))
    result = wait(manager, response.json()['id'])
    assert result['status'] == 'paused_storage' and '100 MB' in result['error']


def test_resume_checkpoint_preserves_finished_results_and_settings(console, monkeypatch):
    client, manager, catalog = console
    response = upload_batch(client, data={'settings': json.dumps(settings(catalog))}, files=[
        ('files', ('first.jpg', b'ok')), ('files', ('bad.jpg', b'bad')), ('files', ('wait.jpg', b'ok'))])
    job_id = response.json()['id']
    deadline = time.monotonic() + 5
    while manager.current()['completed'] < 2 and time.monotonic() < deadline:
        time.sleep(.03)
    assert manager.current()['completed'] == 2
    result = manager.root / job_id / 'output/000000/result.json'
    original = (result.read_bytes(), result.stat().st_mtime_ns)
    with monkeypatch.context() as patch:
        def full(*args):
            raise engine.JobError(507, '存储空间不足：需保留超过 100 MB')
        patch.setattr(jobs, 'require_space', full)
        paused = wait(manager, job_id)
        assert paused['status'] == 'paused_storage' and paused['completed'] == 2
        manager._jobs[job_id].thread.join(2)
        assert [i['status'] for i in paused['items']] == ['success', 'failed', 'pending']
        assert client.post(f'{BASE}/jobs/{job_id}/resume').status_code == 507
    # A process restart and clearing finished records both preserve this batch.
    restored = jobs.ImageJobs(manager.root, catalog)
    assert restored.current() == paused
    monkeypatch.setattr(routes, 'image_jobs', restored)
    try:
        assert client.delete(BASE + '/jobs/completed').json()['cleared'] == 0
        with monkeypatch.context() as patch:
            patch.setattr(jobs, 'npu_busy', lambda: True)
            assert client.post(f'{BASE}/jobs/{job_id}/resume').status_code == 409
        assert client.post(f'{BASE}/jobs/{job_id}/resume').status_code == 202
        completed = wait(restored, job_id)
        assert completed['status'] == 'completed_with_errors'
        assert completed['settings'] == paused['settings']
        assert completed['completed'] == 3 and not completed['error']
        assert (result.read_bytes(), result.stat().st_mtime_ns) == original
        assert (result.parent.parent / 'executed.log').read_text().splitlines() == ['0', '1', '2']
        assert client.post(f'{BASE}/jobs/{job_id}/resume').status_code == 409
    finally:
        restored.shutdown()


def test_device_copy_resumes_after_space_failure(console, tmp_path, monkeypatch):
    client, manager, catalog = console
    folder = tmp_path / 'pictures'; folder.mkdir()
    for name in ('first.jpg', 'second.jpg'):
        (folder / name).write_bytes(b'ok')
    copy_image = jobs.snapshot_device_image
    def full_on_second(image, target, checkpoint):
        if image.name == 'second.jpg':
            target.write_bytes(b'o')
            raise engine.JobError(507, '存储空间不足')
        copy_image(image, target, checkpoint)
    with monkeypatch.context() as patch:
        patch.setattr(jobs, 'snapshot_device_image', full_on_second)
        response = client.post(BASE + '/jobs/from-device', json=dict(path=str(folder), settings=settings(catalog)))
    assert response.status_code == 507
    job = manager.current()
    assert job['status'] == 'paused_storage' and job['resume_phase'] == 'upload'
    original = manager.root / job['id'] / 'input/000000.jpg'
    timestamp = original.stat().st_mtime_ns
    (folder / 'first.jpg').unlink()  # Already copied inputs no longer depend on originals.
    assert client.post(f"{BASE}/jobs/{job['id']}/resume").status_code == 202
    assert wait(manager, job['id'])['status'] == 'completed'
    assert original.stat().st_mtime_ns == timestamp


def test_cancel_paused_job_releases_slot(console):
    client, manager, catalog = console
    job_id = manager.reserve(settings(catalog), ['x.jpg'], sizes=[2])
    manager.pause_upload(job_id, 0, '存储空间不足')
    assert client.post(f'{BASE}/jobs/{job_id}/cancel').json()['status'] == 'cancelled'
    assert client.post(f'{BASE}/jobs/{job_id}/resume').status_code == 409
    assert manager.reserve(settings(catalog), ['next.jpg'], sizes=[2])


def test_full_disk_checkpoint_releases_reserve(console, monkeypatch):
    _, manager, catalog = console
    job_id = manager.reserve(settings(catalog), ['x.jpg'], sizes=[2])
    job = manager._jobs[job_id]
    write = jobs.write_json
    def full(path, value):
        if (job.directory / 'checkpoint.reserve').exists():
            raise OSError(errno.ENOSPC, 'No space left on device')
        write(path, value)
    monkeypatch.setattr(jobs, 'write_json', full)
    manager.pause_upload(job_id, 0, '存储空间不足')
    assert jobs.read_json(job.directory / 'status.json')['status'] == 'paused_storage'
    assert not (job.directory / 'checkpoint.reserve').exists()
    monkeypatch.setattr(jobs, 'write_json', write)
    assert manager.resume(job_id)['status'] == 'uploading'
    assert (job.directory / 'checkpoint.reserve').is_file()
    monkeypatch.setattr(jobs, 'write_json', full)
    manager.pause_upload(job_id, 0, '再次空间不足')
    assert jobs.read_json(job.directory / 'status.json')['status'] == 'paused_storage'


def test_restart_discards_only_incomplete_upload_and_can_continue(console, monkeypatch):
    client, manager, catalog = console
    job_id = reserve_upload(client, catalog, [dict(name='first.jpg', size=2), dict(name='second.jpg', size=2)]).json()['id']
    assert client.put(f'{BASE}/jobs/{job_id}/inputs/0', content=b'ok').status_code == 200
    partial = manager.input_path(job_id, 1)
    partial.write_bytes(b'o')
    restored = jobs.ImageJobs(manager.root, catalog)
    monkeypatch.setattr(routes, 'image_jobs', restored)
    try:
        assert restored.current()['status'] == 'paused_storage'
        assert restored.current()['items'][0]['uploaded']
        assert not partial.exists()
        assert client.post(f'{BASE}/jobs/{job_id}/resume').json()['status'] == 'uploading'
        assert client.put(f'{BASE}/jobs/{job_id}/inputs/1', content=b'ok').status_code == 200
        assert client.post(f'{BASE}/jobs/{job_id}/submit').status_code == 202
        assert wait(restored, job_id)['status'] == 'completed'
    finally:
        restored.shutdown()
