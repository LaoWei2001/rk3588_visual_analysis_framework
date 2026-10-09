import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile

import pytest
from fastapi import HTTPException
from routers import dataset
from services import receiver_package
from services.dataset_manager import DatasetManager


def base_executable():
    data = bytearray(256)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3c, 0x80)
    data[0x80:0x86] = b"PE\x00\x00\x64\x86"
    return bytes(data)


@pytest.fixture
def artifact(tmp_path, monkeypatch):
    resources = tmp_path / "resources"
    directory = resources / "receiver/windows-amd64"
    directory.mkdir(parents=True)
    source = resources / "dataset_receiver.py"
    source.write_text("print('receiver')\n")
    binary = directory / "dataset_receiver.exe"
    binary.write_bytes(base_executable())
    manifest = {"format": 1, "platform": "windows-amd64",
                "receiver_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
    (directory / "manifest.json").write_text(json.dumps(manifest))
    monkeypatch.setattr(receiver_package, "RESOURCES", resources)
    receiver_package._validate.cache_clear()
    return resources, binary


def test_windows_download_is_universal_and_needs_no_credentials(artifact):
    _, binary = artifact
    response = dataset.receiver_package(dataset.ReceiverPackage(platform='windows-amd64'))
    assert response.media_type == 'application/octet-stream'
    assert response.headers['cache-control'] == 'no-store'
    assert response.headers['content-disposition'].endswith('.exe"')
    assert response.body == binary.read_bytes()
    repeat = dataset.receiver_package(dataset.ReceiverPackage(url='https://other-device/控制台/', platform='windows-amd64'))
    assert repeat.body == response.body


def test_missing_exe_preserves_existing_receiver(artifact, monkeypatch):
    resources, binary = artifact
    manager = DatasetManager(resources.parent / 'board')
    monkeypatch.setattr(dataset, 'manager', manager)
    manager.heartbeat('computer-a', 'D:/samples', '')
    lease = manager.settings('receiver')
    binary.unlink()
    with pytest.raises(HTTPException) as error:
        dataset.receiver_package(dataset.ReceiverPackage(platform='windows-amd64'))
    assert error.value.status_code == 503
    assert '部署' in error.value.detail
    assert manager.settings('receiver') == lease


def test_artifact_replaced_after_cached_validation_is_rechecked(artifact):
    _, binary = artifact
    assert receiver_package.windows_receiver() == binary
    binary.write_bytes(binary.read_bytes() + b"modified")
    with pytest.raises(ValueError, match="校验失败"):
        receiver_package.windows_receiver()


def test_stale_receiver_code_is_not_distributed(artifact):
    resources, _ = artifact
    receiver_package.windows_receiver()
    (resources / "dataset_receiver.py").write_text("print('updated')\n")
    with pytest.raises(ValueError, match="源码已更新"):
        receiver_package.windows_receiver()


def test_wrong_architecture_is_rejected(artifact):
    _, binary = artifact
    data = bytearray(binary.read_bytes())
    data[0x84:0x86] = b"\x4c\x01" # 32-bit PE
    binary.write_bytes(data)
    with pytest.raises(ValueError, match="64 位"):
        receiver_package.windows_receiver()


def test_python_download_contains_address_but_no_credentials():
    response = dataset.receiver_package(dataset.ReceiverPackage(url='http://device', platform='python'))
    with zipfile.ZipFile(io.BytesIO(response.body)) as archive:
        assert 'dataset_receiver.py' in archive.namelist()
        assert json.loads(archive.read('receiver.json')) == {'url': 'http://device'}


def test_downloads_preserve_receiver_and_save_confirmations(artifact, tmp_path, monkeypatch):
    manager = DatasetManager(tmp_path / 'board')
    monkeypatch.setattr(dataset, 'manager', manager)
    manager.heartbeat('computer-a', 'D:/采集图片', '')
    lease = manager.settings('receiver')
    proof = manager.proof('task', 'revision', 'sample')
    for platform in ['windows-amd64', 'python']:
        dataset.receiver_package(dataset.ReceiverPackage(url='http://device', platform=platform))
    assert manager.settings('receiver') == lease
    assert manager.proof('task', 'revision', 'sample') == proof
    assert DatasetManager(manager.root).proof('task', 'revision', 'sample') == proof
    assert manager.receiver_status()['online']
    assert manager.settings('token') is None


def test_concurrent_save_confirmations_share_one_internal_key(tmp_path):
    from concurrent.futures import ThreadPoolExecutor
    manager = DatasetManager(tmp_path / 'board')
    with ThreadPoolExecutor(max_workers=4) as executor:
        proofs = list(executor.map(lambda _: manager.proof('task', 'revision', 'sample'), range(8)))
    assert len(set(proofs)) == 1
    assert DatasetManager(manager.root).proof('task', 'revision', 'sample') == proofs[0]
    assert manager.settings('token') is None


def test_upgrade_preserves_pending_image_confirmations(tmp_path):
    import hmac
    manager = DatasetManager(tmp_path / 'board')
    manager.settings('token', 'legacy-device-token')
    old_proof = hmac.new(b'legacy-device-token', json.dumps(['task', 'revision', 'sample']).encode(), hashlib.sha256).hexdigest()
    assert manager.proof('task', 'revision', 'sample') == old_proof
    assert DatasetManager(manager.root).proof('task', 'revision', 'sample') == old_proof


def test_checked_real_binary_is_packaged_when_present(monkeypatch):
    directory = Path(dataset.__file__).resolve().parents[1] / "resources/receiver/windows-amd64"
    if not (directory / "dataset_receiver.exe").exists():
        pytest.skip("Build the Windows receiver first")
    response = dataset.receiver_package(dataset.ReceiverPackage(url="https://device", platform="windows-amd64"))
    assert len(response.body) > 1024 * 1024
    assert response.body[:2] == b"MZ"
    assert response.body == receiver_package.windows_receiver().read_bytes()
