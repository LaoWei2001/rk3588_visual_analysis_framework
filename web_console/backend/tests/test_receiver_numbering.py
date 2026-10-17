"""Exercise folder allocation using real files and the receiver's durable DB."""
import importlib.util
from pathlib import Path
import shutil

import pytest

SOURCE = Path(__file__).resolve().parents[1] / 'resources/dataset_receiver.py'
spec = importlib.util.spec_from_file_location('numbering_receiver', SOURCE)
pc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pc)
JPEG = b'\xff\xd8test\xff\xd9'


def sample(index, *, app='demo', channel=0, batch=30):
    return dict(sample_id=f'{app}-{channel}-{index}', task_id='task', revision='v1', app=app,
                channel_id=channel, images_per_folder=batch, bytes=len(JPEG), proof='proof')


@pytest.fixture
def receiver(tmp_path):
    receiver = pc.Receiver('http://device', tmp_path)
    receiver.request = lambda *args, **kwargs: JPEG
    yield receiver
    receiver.db.close()


def receive_all(receiver, count):
    paths = [receiver.receive(sample(i)) for i in range(count)]
    with receiver.db:
        receiver.db.execute('UPDATE images SET acknowledged=1')
    return paths


@pytest.mark.parametrize('delete', ['pictures', 'batches', 'channel', 'device'])
@pytest.mark.parametrize('restart', [False, True])
def test_600_deleted_photos_restart_at_zero(receiver, delete, restart):
    paths = receive_all(receiver, 600)
    assert paths[0].parent.name == '0_29' and paths[-1].parent.name == '570_599'
    directory = paths[0].parent.parent
    client = receiver.client
    if delete == 'pictures':
        for path in paths: path.unlink()
    elif delete == 'batches':
        for path in directory.iterdir(): shutil.rmtree(path)
    elif delete == 'channel': shutil.rmtree(directory)
    else: shutil.rmtree(directory.parent.parent)
    if restart:
        # The historical cursor and ACKs remain in the existing SQLite file.
        other = pc.Receiver(receiver.url, receiver.directory)
        other.request = receiver.request
    else:
        other = receiver
    try:
        path = other.receive(sample(600))
        assert path.parent.name == '0_29'
        assert sorted(child.name for child in directory.iterdir()) == ['0_29']
        assert other.client == client
        assert other.db.execute('SELECT COUNT(*) FROM images WHERE acknowledged=1').fetchone()[0] == 600
        following = [other.receive(sample(i)) for i in range(601, 631)]
        assert following[28].parent.name == '0_29'
        assert following[29].parent.name == '30_59'
        assert len(list((directory / '0_29').glob('*.jpg'))) == 30
    finally:
        if restart: other.db.close()


def test_partial_deletion_keeps_numbering_and_remaining_photos(receiver):
    paths = receive_all(receiver, 60)
    kept = paths[0]
    for path in paths[1:]: path.unlink()
    result = receiver.receive(sample(60))
    assert result.parent.name == '60_89'
    assert kept.read_bytes() == JPEG


def test_unrelated_files_are_preserved_and_do_not_advance_the_new_sequence(receiver):
    paths = receive_all(receiver, 60)
    notes = paths[-1].parent / 'notes.txt'
    notes.write_text('keep me')
    for path in paths: path.unlink()
    assert receiver.receive(sample(60)).parent.name == '0_29'
    assert receiver.receive(sample(61)).parent.name == '0_29'
    assert notes.read_text() == 'keep me'


def test_other_channels_apps_and_devices_keep_their_cursors(receiver):
    paths = receive_all(receiver, 30)
    other_channel = receiver.receive(sample(0, channel=3, batch=1))
    other_app = receiver.receive(sample(0, app='other', batch=1))
    another = pc.Receiver('http://other-device', receiver.directory)
    another.request = receiver.request
    try:
        other_device = another.receive(sample(0, batch=1))
        for path in paths: path.unlink()
        assert receiver.receive(sample(30)).parent.name == '0_29'
        assert receiver.receive(sample(1, channel=3, batch=1)).parent.name == '1_1'
        assert receiver.receive(sample(1, app='other', batch=1)).parent.name == '1_1'
        assert another.receive(sample(1, batch=1)).parent.name == '1_1'
        assert all(path.read_bytes() == JPEG for path in [other_channel, other_app, other_device])
    finally:
        another.db.close()


def test_failed_download_reservation_and_retry_are_preserved(receiver):
    paths = receive_all(receiver, 30)
    def interrupted(*args, **kwargs): raise OSError('disconnected')
    receiver.request = interrupted
    with pytest.raises(OSError): receiver.receive(sample(30))
    for path in paths: path.unlink()
    receiver.request = lambda *args, **kwargs: JPEG
    # A reserved but unwritten sample blocks resetting until it can be saved.
    assert receiver.receive(sample(31)).parent.name == '30_59'
    path = receiver.receive(sample(30))
    assert path.parent.name == '30_59'
    assert receiver.receive(sample(30)) == path
    assert len(list(path.parent.glob('*.jpg'))) == 2


def test_partial_download_and_renamed_photos_prevent_reset(receiver):
    paths = receive_all(receiver, 30)
    renamed = paths[0].with_name('renamed.JPEG')
    paths[0].rename(renamed)
    for path in paths[1:]: path.unlink()
    assert receiver.receive(sample(30)).parent.name == '30_59'
    renamed.unlink()
    next(receiver.directory.rglob('*.jpg')).unlink()
    partial = paths[-1].with_suffix('.jpg.part')
    partial.write_bytes(b'incomplete')
    assert receiver.receive(sample(31)).parent.name == '31_60'
    assert partial.read_bytes() == b'incomplete'


def test_batch_change_and_duplicate_receive_do_not_overwrite_photos(receiver):
    first = receiver.receive(sample(0))
    assert receiver.receive(sample(0)) == first
    changed = receiver.receive(sample(1, batch=10))
    assert changed.parent.name == '30_39'
    assert first.read_bytes() == JPEG


def test_unreadable_directory_does_not_silently_reset(receiver, monkeypatch):
    receive_all(receiver, 30)
    def denied(directory): raise PermissionError('unavailable')
    monkeypatch.setattr(receiver, '_has_capture_file', denied)
    with pytest.raises(PermissionError): receiver.receive(sample(30))
    assert receiver.db.execute('SELECT next FROM groups').fetchone()[0] == 30
