"""Real Unix capture server + HTTP router + PC receiver, without another model run."""
import importlib.util
import asyncio
import io
import json
import signal
import socket
import subprocess
import threading
import time
import zipfile
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, build_opener, ProxyHandler

import pytest
import uvicorn
from fastapi import HTTPException
from routers import dataset
from services.dataset_manager import DatasetManager

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('pc_receiver',ROOT/'web_console/backend/resources/dataset_receiver.py')
pc=importlib.util.module_from_spec(spec);spec.loader.exec_module(pc)

@pytest.fixture
def environment(tmp_path,monkeypatch):
    binary=ROOT/'build/engine/remote_dataset_tests'
    if not binary.is_file(): pytest.skip('Build remote_dataset_tests first')
    board=tmp_path/'board';app=board/'demo';assets=app/'assets';assets.mkdir(parents=True)
    (assets/'labels.txt').write_text('person\nhelmet\n')
    (assets/'config.json').write_text(json.dumps({'channels':[
        {'id':channel,'logic':'logic_crane_helmet','models':[{'enable':True,'model_type':'yolov8_det','label_path':'assets/labels.txt'}]}
        for channel in [0,3]]}))
    manager=DatasetManager(board)
    monkeypatch.setattr(dataset,'manager',manager)
    process=subprocess.Popen([str(binary),str(app/'run.dataset.sock')],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    for _ in range(100):
        if (app/'run.dataset.sock').exists(): break
        if process.poll() is not None: pytest.fail(process.stderr.read().decode())
        time.sleep(.02)
    assert (app/'run.dataset.sock').exists()
    manager.fixture_process=process
    yield manager,board,tmp_path/'computer'
    manager.fixture_process.terminate();manager.fixture_process.wait(timeout=5)

def config(limit=3,batch=2):
    return dataset.TaskConfig(rules=[{'id':'r1','name':'有人时采集','condition':{
        'class':'person','min_confidence':.3,'count':{'op':'>=','value':1}}}],
        max_samples=limit,images_per_folder=batch,interval_sec=.5).model_dump()

def test_capture_http_receiver_two_channels_and_browser_independence(environment,monkeypatch):
    manager,board,computer=environment
    import main
    from services.auth_service import create_session,revoke_session
    token=create_session('integration-user')
    listener=socket.socket();listener.bind(('127.0.0.1',0));port=listener.getsockname()[1]
    server=uvicorn.Server(uvicorn.Config(main.app,log_level='error',lifespan='off'))
    worker=threading.Thread(target=lambda:server.run(sockets=[listener]),daemon=True);worker.start()
    for _ in range(100):
        if server.started: break
        time.sleep(.02)
    url=f'http://127.0.0.1:{port}'
    opener=build_opener(ProxyHandler({}))
    def api(path,body=None,method=None,auth=token,binary=False):
        data=json.dumps(body).encode() if body is not None else None
        headers={'Content-Type':'application/json'}
        if auth: headers['Authorization']='Bearer '+auth
        request=Request(url+'/api/'+path,data=data,method=method,headers=headers)
        with opener.open(request,timeout=8) as response:
            result=response.read()
            return result if binary else json.loads(result)
    try:
        result=api('apps/demo/dataset/channels/0',config(),method='PUT')
        task=next(item for item in result['tasks'] if item['channel_id']==0)
        api(f'apps/demo/dataset/tasks/{task["id"]}/enabled',{'enabled':True})
        package=api('dataset/receiver-package',{'url':url},binary=True)
        with zipfile.ZipFile(io.BytesIO(package)) as archive:
            connection=json.loads(archive.read('receiver.json'))
            assert connection=={'url':url}
            assert 'dataset_receiver.py' in archive.namelist() and '启动接收.cmd' in archive.namelist()
        receiver=pc.Receiver(url.removeprefix('http://'),computer);receiver.opener=opener
        receiver.cycle()
        assert manager.receiver_status()['online']
        # Add another channel after this receiver is already connected. The same
        # process must discover it on the next poll, without any credentials.
        result=api('apps/demo/dataset/channels/3',config(),method='PUT')
        task=next(item for item in result['tasks'] if item['channel_id']==3)
        api(f'apps/demo/dataset/tasks/{task["id"]}/enabled',{'enabled':True})
        lease=manager.settings('receiver')
        package=api('dataset/receiver-package',{'url':url},binary=True)
        with zipfile.ZipFile(io.BytesIO(package)) as archive:
            assert json.loads(archive.read('receiver.json'))==connection
        assert manager.settings('receiver')==lease
        # Logout/expired normal session must not stop the PC receiver.
        revoke_session(token)
        with pytest.raises(HTTPError) as rejected: api('apps/demo/dataset')
        assert rejected.value.code==401
        for path, body, method in [
            ('apps/demo/dataset/channels/3', config(), 'PUT'),
            (f'apps/demo/dataset/tasks/{task["id"]}/enabled', {'enabled':False}, 'POST'),
            ('dataset/receiver-package', {'url':url}, 'POST'),
            (f'apps/demo/dataset/tasks/{task["id"]}', None, 'DELETE'),
            # Sharing the transfer URL prefix must not make future routes public.
            ('dataset-receiver/settings', None, 'GET'),
            ('dataset-receiver/poll', None, 'GET'),
            ('dataset-receiver/disconnect', None, 'GET'),
        ]:
            with pytest.raises(HTTPError) as rejected: api(path,body,method,auth=None)
            assert rejected.value.code==401
        assert 'samples' in api('dataset-receiver/poll',{'client':receiver.client},auth=None)
        # Old EXEs still send their former token. It is ignored for transfers,
        # and must not authorize task configuration or other console APIs.
        assert 'samples' in api('dataset-receiver/poll',{'client':receiver.client},auth='old-receiver-token')
        with pytest.raises(HTTPError) as rejected: api('apps/demo/dataset',auth='old-receiver-token')
        assert rejected.value.code==401
    except Exception:
        server.should_exit=True;worker.join(timeout=5);listener.close()
        raise
    try:
        end=time.monotonic()+12
        while time.monotonic()<end:
            receiver.cycle()
            if sum(row['saved'] for row in manager.rows())==6: break
            time.sleep(.15)
        rows=manager.rows();assert [row['saved'] for row in rows]==[3,3]
        files=list(computer.rglob('*.jpg'));assert len(files)==6
        for channel in [0,3]:
            folders=list(computer.glob(f'*/demo/ch{channel}/*'))
            assert sorted(path.name for path in folders)==['0_1','2_3']
            assert sorted(len(list(path.glob('*.jpg'))) for path in folders)==[1,2]
        assert list(board.rglob('*.jpg'))==[]
        # Retries/restart do not duplicate files or saved counts.
        receiver.cycle();receiver.db.close()
        restart=pc.Receiver(url,computer);restart.opener=opener
        assert restart.client==receiver.client
        restart.cycle();assert len(list(computer.rglob('*.jpg')))==6
        assert [row['saved'] for row in DatasetManager(board).rows()]==[3,3]
        assert all(task['pending']==0 for task in manager.list('demo')['tasks'])
        # A fresh engine restores persisted progress and does not collect another quota.
        manager.fixture_process.terminate();manager.fixture_process.wait(timeout=5)
        manager.fixture_process=subprocess.Popen([str(ROOT/'build/engine/remote_dataset_tests'),str(board/'demo/run.dataset.sock')],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        for _ in range(100):
            if (board/'demo/run.dataset.sock').exists(): break
            time.sleep(.02)
        restart.cycle()
        assert [row['saved'] for row in manager.rows()]==[3,3]
        assert all(task['pending']==0 for task in manager.list('demo')['tasks'])
        # Real file scans travel through the unauthenticated HTTP heartbeat.
        monkeypatch.setattr(pc.InventoryScanner,'INTERVAL_SECONDS',.03)
        restart.inventory.start()
        try:
            deadline=time.monotonic()+3
            while restart.inventory.snapshot() is None and time.monotonic()<deadline:
                time.sleep(.02)
            restart.cycle()
            assert [task['current_files'] for task in manager.list('demo')['tasks']]==[3,3]
            deleted=next(computer.glob('*/demo/ch0/*/*.jpg'));deleted.unlink()
            deadline=time.monotonic()+3
            while time.monotonic()<deadline:
                restart.cycle()
                if manager.list('demo')['tasks'][0]['current_files']==2: break
                time.sleep(.03)
            assert [task['current_files'] for task in manager.list('demo')['tasks']]==[2,3]
            assert [row['saved'] for row in manager.rows()]==[3,3]
            assert all(task['pending']==0 for task in manager.list('demo')['tasks'])
        finally: restart.inventory.stop()
        restart.db.close()
        assert 'samples' in receiver.request('poll',{'client':receiver.client})
        receiver.disconnect()
        assert not manager.receiver_status()['online']
        # Releasing the old lease lets another directory connect immediately.
        replacement=pc.Receiver(url,computer/'new-directory');replacement.opener=opener
        try:
            replacement.cycle()
            assert manager.receiver_status()['online']
            lease=manager.settings('receiver')
            assert api('dataset-receiver/disconnect',{'client':receiver.client},auth=None)=={'disconnected':False}
            assert manager.settings('receiver')==lease
            assert all(task['current_files'] is None for task in manager.list('demo')['tasks'])
            replacement.disconnect()
            assert not manager.receiver_status()['online']
        finally: replacement.db.close()
    finally:
        server.should_exit=True;worker.join(timeout=5);listener.close()

def test_ack_is_durable_idempotent_and_recovers_failed_native_ack(environment,monkeypatch):
    manager,board,computer=environment
    result=manager.save('demo',0,config(limit=1))
    task=result['tasks'][0];manager.set_enabled('demo',task['id'],True)
    end=time.monotonic()+3;pending=[]
    while time.monotonic()<end and not pending:
        pending=manager.pending();time.sleep(.02)
    sample=pending[0]
    saved_ack={key:sample[key] for key in ['app','task_id','revision','sample_id','proof']}
    actual_rpc=manager.rpc
    def offline_ack(app,request):
        if request['op']=='ack': raise HTTPException(503,'simulated disconnect after durable commit')
        return actual_rpc(app,request)
    monkeypatch.setattr(manager,'rpc',offline_ack)
    assert manager.acknowledge(**saved_ack)==1
    assert manager.acknowledge(**saved_ack)==1
    monkeypatch.setattr(manager,'rpc',actual_rpc)
    assert manager.pending()==[] # Reconciliation releases an already acknowledged JPEG.
    assert DatasetManager(board).rows()[0]['saved']==1
    saved_ack['proof']='0'*64
    with pytest.raises(HTTPException) as invalid: manager.acknowledge(**saved_ack)
    assert invalid.value.status_code==403

def test_config_validation_pause_and_no_second_receiver(environment):
    manager,board,computer=environment
    invalid=config();invalid['rules'][0]['condition']['class']='unknown'
    with pytest.raises(HTTPException) as rejected: manager.save('demo',0,invalid)
    assert rejected.value.status_code==409 and manager.rows()==[]
    result=manager.save('demo',0,config());task=result['tasks'][0]
    manager.set_enabled('demo',task['id'],True)
    with pytest.raises(HTTPException): manager.save('demo',0,config())
    manager.set_enabled('demo',task['id'],False)
    manager.heartbeat('computer-a','D:/samples','')
    with pytest.raises(HTTPException) as conflict: manager.heartbeat('computer-b','D:/other','')
    assert conflict.value.status_code==409
    assert manager.receiver_status()['online']

def test_duration_stops_and_changing_duration_restarts_clock(environment):
    manager,board,computer=environment
    timed=config(limit=0);timed['duration_hours']=.00003
    result=manager.save('demo',0,timed);task=result['tasks'][0]
    manager.set_enabled('demo',task['id'],True)
    time.sleep(.2)
    assert manager.list('demo')['tasks'][0]['status']=='已达到运行时长'
    manager.set_enabled('demo',task['id'],False)
    timed['duration_hours']=2
    manager.save('demo',0,timed)
    assert manager.rows()[0]['deadline']==0
    manager.set_enabled('demo',task['id'],True)
    assert manager.rows()[0]['deadline']>time.time()+7100


def restart_engine(manager, board):
    manager.fixture_process=subprocess.Popen([str(ROOT/'build/engine/remote_dataset_tests'),str(board/'demo/run.dataset.sock')],
                                             stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    for _ in range(100):
        if (board/'demo/run.dataset.sock').exists(): return
        if manager.fixture_process.poll() is not None: pytest.fail(manager.fixture_process.stderr.read().decode())
        time.sleep(.02)
    pytest.fail('restarted capture socket did not become ready')


@pytest.mark.parametrize('enabled',[False,True])
def test_delete_task_with_stopped_engine_and_no_resurrection(environment,enabled):
    manager,board,computer=environment
    task=manager.save('demo',0,config())['tasks'][0]
    if enabled: manager.set_enabled('demo',task['id'],True)
    computer.mkdir();saved_image=computer/'kept.jpg';saved_image.write_bytes(b'\xff\xd8\xff\xd9')
    with manager.db() as db:
        db.execute('INSERT INTO acknowledgments VALUES (?,?)',('previous-image',task['id']))
    manager.fixture_process.terminate();manager.fixture_process.wait(timeout=5)
    result=dataset.delete_task('demo',task['id'])
    assert result['tasks']==[] and manager.rows()==[]
    assert saved_image.is_file()
    with manager.db() as db:
        assert db.execute('SELECT COUNT(*) FROM acknowledgments').fetchone()[0]==0
        assert db.execute('SELECT COUNT(*) FROM task_removals').fetchone()[0]==1
    # Pending deletion survives backend restart; engine restart must not restore it.
    restored=DatasetManager(board)
    restart_engine(manager,board)
    response,error=restored.sync('demo')
    assert not error and response['tasks']==[] and restored.list('demo')['tasks']==[]
    with restored.db() as db:
        assert db.execute('SELECT COUNT(*) FROM task_removals').fetchone()[0]==0


def test_offline_delete_during_socket_outage_cleans_live_cache_on_reconnect(environment,monkeypatch):
    manager,board,computer=environment
    task=manager.save('demo',0,config(limit=0))['tasks'][0]
    manager.set_enabled('demo',task['id'],True)
    manager.save('demo',3,config())
    deadline=time.monotonic()+3
    while time.monotonic()<deadline and not manager.pending(): time.sleep(.02)
    assert manager.pending()
    def unavailable(*args): raise HTTPException(503,'simulated transport outage')
    with monkeypatch.context() as patch:
        patch.setattr(manager,'rpc',unavailable)
        assert len(manager.remove('demo',task['id'])['tasks'])==1
    restored=DatasetManager(board)
    actual=restored.rpc
    failed_once=[]
    def interrupt_cleanup(app,command):
        if command['op']=='remove' and not failed_once:
            failed_once.append(True)
            raise HTTPException(503,'simulated cleanup disconnect')
        return actual(app,command)
    with monkeypatch.context() as patch:
        patch.setattr(restored,'rpc',interrupt_cleanup)
        _,error=restored.sync('demo')
        assert error=='simulated cleanup disconnect'
    with restored.db() as db:
        assert db.execute('SELECT COUNT(*) FROM task_removals').fetchone()[0]==1
    deadline=time.monotonic()+3
    while time.monotonic()<deadline:
        response,error=restored.sync('demo')
        if not error and all(row['task_id']!=task['id'] for row in response['tasks']): break
        time.sleep(.03)
    assert not error and [row['channel_id'] for row in response['tasks']]==[3]
    assert [row['channel'] for row in restored.rows()]==[3]
    assert restored.rows()[0]['saved']==0
    with restored.db() as db:
        assert db.execute('SELECT COUNT(*) FROM task_removals').fetchone()[0]==0
        assert db.execute('SELECT COUNT(*) FROM acknowledgments').fetchone()[0]==0


def test_running_task_and_pending_images_still_require_pause_and_transfer(environment):
    manager,board,computer=environment
    task=manager.save('demo',0,config(limit=1))['tasks'][0]
    manager.set_enabled('demo',task['id'],True)
    with pytest.raises(HTTPException) as rejected: manager.remove('demo',task['id'])
    assert rejected.value.status_code==409 and '暂停' in rejected.value.detail
    deadline=time.monotonic()+3
    while time.monotonic()<deadline and not manager.pending(): time.sleep(.02)
    assert manager.pending()
    manager.set_enabled('demo',task['id'],False)
    with pytest.raises(HTTPException) as rejected: manager.remove('demo',task['id'])
    assert rejected.value.status_code==409 and '待传' in rejected.value.detail
    assert len(manager.rows())==1
    with manager.db() as db:
        assert db.execute('SELECT COUNT(*) FROM task_removals').fetchone()[0]==0


def test_deleted_task_cleanup_preserves_replacement_on_same_channel(environment,monkeypatch):
    manager,board,computer=environment
    old=manager.save('demo',0,config())['tasks'][0]
    def unavailable(*args): raise HTTPException(503,'simulated transport outage')
    with monkeypatch.context() as patch:
        patch.setattr(manager,'rpc',unavailable)
        manager.remove('demo',old['id'])
    replacement=manager.save('demo',0,config())['tasks'][0]
    assert replacement['id']!=old['id']
    response,error=manager.sync('demo')
    assert not error and [task['task_id'] for task in response['tasks']]==[replacement['id']]
    assert [task['id'] for task in manager.rows()]==[replacement['id']]
    with manager.db() as db:
        assert db.execute('SELECT COUNT(*) FROM task_removals').fetchone()[0]==0


@pytest.mark.parametrize('status',[404,409])
def test_delete_does_not_mask_other_errors(environment,monkeypatch,status):
    manager,board,computer=environment
    task=manager.save('demo',0,config())['tasks'][0]
    def rejected(*args): raise HTTPException(status,'simulated rejection')
    monkeypatch.setattr(manager,'rpc',rejected)
    with pytest.raises(HTTPException) as error: manager.remove('demo',task['id'])
    assert error.value.status_code==status and len(manager.rows())==1
    with manager.db() as db:
        assert db.execute('SELECT COUNT(*) FROM task_removals').fetchone()[0]==0


def test_maintenance_retries_deletion_when_last_task_is_gone(tmp_path,monkeypatch):
    manager=DatasetManager(tmp_path)
    with manager.db() as db:
        db.execute('INSERT INTO task_removals VALUES (?,?,?)',('deleted','demo','{}'))
    observed=threading.Event()
    def sync(app):
        assert app=='demo'
        observed.set()
    monkeypatch.setattr(manager,'sync',sync)
    async def check():
        maintenance=asyncio.create_task(manager.maintenance())
        try: assert await asyncio.to_thread(observed.wait,2)
        finally:
            maintenance.cancel()
            with pytest.raises(asyncio.CancelledError): await maintenance
    asyncio.run(check())

def test_pc_disk_failure_preserves_image_and_never_acknowledges_early(environment,monkeypatch):
    from urllib.parse import parse_qs
    manager,board,computer=environment
    task=manager.save('demo',0,config(limit=1))['tasks'][0]
    manager.set_enabled('demo',task['id'],True)
    end=time.monotonic()+3;samples=[]
    while time.monotonic()<end and not samples:
        samples=manager.pending();time.sleep(.02)
    sample=samples[0];receiver=pc.Receiver('http://device',computer)
    def relay(path,body=None,binary=False):
        if path.startswith('image?'):
            query={key:value[0] for key,value in parse_qs(path.split('?',1)[1]).items()}
            _,image=manager.rpc(query['app'],{'op':'image','task_id':query['task_id'],'sample_id':query['sample_id']})
            return image
        if path=='ack': return {'saved':manager.acknowledge(**body)}
        raise AssertionError(path)
    receiver.request=relay
    original=pc.shutil.disk_usage
    monkeypatch.setattr(pc.shutil,'disk_usage',lambda _: type('Disk',(),{'free':0})())
    with pytest.raises(OSError): receiver.receive(sample)
    assert manager.rows()[0]['saved']==0 and len(manager.pending())==1
    assert not list(computer.rglob('*.jpg'))
    monkeypatch.setattr(pc.shutil,'disk_usage',original)
    path=receiver.receive(sample)
    assert path.is_file() and receiver.receive(sample)==path
    receiver.acknowledge_pending()
    assert manager.rows()[0]['saved']==1 and manager.pending()==[]
    assert len(list(computer.rglob('*.jpg')))==1
    malicious=dict(sample,app='../outside')
    with pytest.raises(ValueError): receiver.receive(malicious)
    receiver.db.close()


def test_pc_first_launch_remembers_address_and_directory_without_credentials(tmp_path, monkeypatch):
    settings=tmp_path/'receiver.json'
    directory=tmp_path/'采集图片'
    monkeypatch.setattr(pc.sys, 'argv', ['receiver', '--config', str(settings), '--output', str(directory)])
    monkeypatch.setattr('builtins.input', lambda _: 'device:8080/')
    seen=[]
    monkeypatch.setattr(pc.Receiver, 'run', lambda self: seen.append((self.url,self.directory)))
    pc.main()
    assert json.loads(settings.read_text())=={'url':'http://device:8080','directory':str(directory)}
    # Subsequent runs reuse the saved configuration without prompting. Legacy
    # JSON files also remain usable; obsolete credentials are removed on save.
    legacy=json.loads(settings.read_text());legacy['token']='obsolete'
    settings.write_text(json.dumps(legacy))
    monkeypatch.setattr(pc.sys, 'argv', ['receiver', '--config', str(settings)])
    monkeypatch.setattr('builtins.input', lambda _: pytest.fail('must remember address and directory'))
    pc.main()
    assert seen==[('http://device:8080',directory)]*2
    assert 'token' not in json.loads(settings.read_text())


@pytest.mark.parametrize('address, expected', [
    (' 192.168.2.41:8080/ ', 'http://192.168.2.41:8080'),
    ('192.168.2.41', 'http://192.168.2.41'),
    ('device', 'http://device'),
    ('device:8080/控制台/', 'http://device:8080/控制台'),
    ('[::1]:8080', 'http://[::1]:8080'),
    ('http://device:8080/', 'http://device:8080'),
    ('https://device:8080/', 'https://device:8080'),
])
def test_pc_address_accepts_optional_http_prefix(address, expected):
    assert pc.normalize_url(address) == expected


@pytest.mark.parametrize('address', [
    '', 'ftp://device', 'device:invalid', 'http://user:password@device',
    'http://device?token=x', 'http://device#fragment', 'device?token=x', 'device#fragment',
])
def test_pc_address_rejects_invalid_input(address):
    with pytest.raises(ValueError):
        pc.normalize_url(address)


@pytest.mark.parametrize('forced', [False, True])
def test_pc_stop_releases_lock_and_reopen_reuses_identity(tmp_path, forced):
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(5)
    url=f'http://127.0.0.1:{listener.getsockname()[1]}'
    directory=tmp_path/'电脑采集'
    process=subprocess.Popen([pc.sys.executable,str(ROOT/'web_console/backend/resources/dataset_receiver.py'),
                              '--url',url,'--output',str(directory),'--config',str(tmp_path/'receiver.json')],
                             stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                             env={**pc.os.environ,'NO_PROXY':'127.0.0.1,localhost','no_proxy':'127.0.0.1,localhost'})
    connection=None
    try:
        # Leave a real HTTP request blocked without responding. Shutdown must
        # work during network I/O, rather than only during the idle sleep.
        connection,_=listener.accept()
        before=pc.Receiver(url,directory)
        client=before.client
        with pytest.raises(RuntimeError):
            with pc.exclusive(directory,before.device): pass
        before.db.close()
        if forced: process.kill()
        else: process.send_signal(signal.SIGINT)
        output,errors=process.communicate(timeout=5)
        if not forced:
            assert process.returncode==0, errors.decode()
            assert '接收已停止' in output.decode()
        reopened=pc.Receiver(url,directory)
        try:
            assert reopened.client==client
            with pc.exclusive(directory,reopened.device): pass
        finally: reopened.db.close()
    finally:
        if process.poll() is None:
            process.kill();process.communicate(timeout=5)
        if connection: connection.close()
        listener.close()


@pytest.mark.parametrize('stop_signal', [signal.SIGINT, signal.SIGTERM])
def test_pc_exit_reports_offline_through_public_http_api(tmp_path, monkeypatch, stop_signal):
    import main
    manager=DatasetManager(tmp_path/'board')
    monkeypatch.setattr(dataset,'manager',manager)
    listener=socket.socket();listener.bind(('127.0.0.1',0))
    url=f'http://127.0.0.1:{listener.getsockname()[1]}'
    server=uvicorn.Server(uvicorn.Config(main.app,log_level='error',lifespan='off'))
    worker=threading.Thread(target=lambda:server.run(sockets=[listener]),daemon=True);worker.start()
    process=None
    try:
        for _ in range(100):
            if server.started: break
            time.sleep(.02)
        assert server.started
        process=subprocess.Popen([pc.sys.executable,str(ROOT/'web_console/backend/resources/dataset_receiver.py'),
                                  '--url',url,'--output',str(tmp_path/'computer'),
                                  '--config',str(tmp_path/'receiver.json')],
                                 stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                 env={**pc.os.environ,'NO_PROXY':'127.0.0.1,localhost','no_proxy':'127.0.0.1,localhost'})
        deadline=time.monotonic()+5
        while time.monotonic()<deadline and not manager.receiver_status()['online']:
            time.sleep(.02)
        assert manager.receiver_status()['online']
        if stop_signal==signal.SIGINT:
            # A duplicate instance that fails the local lock must not send an
            # exit notification for the still-running receiver with the same ID.
            duplicate=subprocess.run([pc.sys.executable,str(ROOT/'web_console/backend/resources/dataset_receiver.py'),
                                      '--url',url,'--output',str(tmp_path/'computer'),
                                      '--config',str(tmp_path/'duplicate.json')],
                                     stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=4)
            assert duplicate.returncode==1 and '已有接收程序运行' in duplicate.stderr.decode()
            assert manager.receiver_status()['online']
        process.send_signal(stop_signal)
        output,errors=process.communicate(timeout=4)
        assert process.returncode==0, errors.decode()
        assert '接收已停止' in output.decode()
        assert not manager.receiver_status()['online']
        assert time.time()-manager.receiver_status()['last_seen']<15
        manager.heartbeat('replacement','D:/other','')
        assert manager.receiver_status()['online']
    finally:
        if process and process.poll() is None:
            process.kill();process.communicate(timeout=5)
        server.should_exit=True;worker.join(timeout=5);listener.close()


def test_disconnect_is_idempotent_and_preserves_current_receiver(tmp_path,monkeypatch):
    manager=DatasetManager(tmp_path/'board')
    assert not manager.disconnect('unknown')
    manager.heartbeat('first','D:/samples','disk error')
    seen=manager.receiver_status()['last_seen']
    assert manager.disconnect('first') and manager.disconnect('first')
    assert manager.receiver_status()=={'online':False,'directory':'D:/samples','error':'','last_seen':seen}
    manager.heartbeat('second','D:/other','')
    state=manager.settings('receiver')
    assert not manager.disconnect('first')
    assert manager.settings('receiver')==state
    # Abrupt exits / old clients keep the existing heartbeat fallback.
    monkeypatch.setattr('services.dataset_manager.time.time',lambda:state['seen']+16)
    assert not manager.receiver_status()['online']
    with pytest.raises(HTTPException): manager.heartbeat('third','D:/third','')
    monkeypatch.setattr('services.dataset_manager.time.time',lambda:state['seen']+31)
    manager.heartbeat('third','D:/third','')
    assert manager.receiver_status()['online']


def test_disconnect_is_bounded_even_if_name_resolution_stalls(tmp_path,monkeypatch):
    receiver=pc.Receiver('http://device',tmp_path/'computer')
    release=threading.Event();entered=threading.Event();finished=threading.Event()
    def stalled(path,body=None,binary=False,*,timeout=20):
        assert path=='disconnect' and body=={'client':receiver.client} and timeout==1
        entered.set()
        release.wait(5)
        finished.set()
    monkeypatch.setattr(receiver,'request',stalled)
    try:
        receiver.disconnect() # No attempted poll: do not touch another receiver.
        assert not entered.is_set()
        receiver._poll_started=True
        started=time.monotonic();receiver.disconnect()
        assert entered.is_set() and not finished.is_set()
        assert time.monotonic()-started<2.5
    finally:
        release.set();finished.wait(2);receiver.db.close()


@pytest.mark.parametrize('code',[401,404,503])
def test_disconnect_failure_does_not_mask_normal_exit(tmp_path,monkeypatch,code):
    receiver=pc.Receiver('http://device',tmp_path/'computer')
    def rejected(*args,**kwargs):
        raise HTTPError('http://device',code,'old or unavailable device',{},None)
    def interrupted():
        receiver._poll_started=True
        raise KeyboardInterrupt
    monkeypatch.setattr(receiver,'request',rejected)
    monkeypatch.setattr(receiver,'_run',interrupted)
    try:
        with pytest.raises(KeyboardInterrupt): receiver.run()
    finally: receiver.db.close()
