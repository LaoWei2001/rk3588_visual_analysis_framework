"""Polling must observe external file writes without expensive inventory scans."""
import subprocess
from services import network_manager, storage_manager


def test_lightweight_storage_read_follows_policy_file_without_scanning_media(tmp_path, monkeypatch):
    monkeypatch.setattr(storage_manager, 'APPS_ROOT', tmp_path)
    monkeypatch.setattr(storage_manager, 'SETTINGS_FILE', tmp_path / 'storage.json')
    monkeypatch.setattr(storage_manager, '_existing_fs_path', lambda: tmp_path)

    def unexpected_scan():
        raise AssertionError('Per-second polling must not scan media or developer caches')

    monkeypatch.setattr(storage_manager, '_events', unexpected_scan)
    monkeypatch.setattr(storage_manager, 'root_cleanup_snapshot', unexpected_scan)
    storage_manager.SETTINGS_FILE.write_text('{"auto_cleanup":false,"retention_days":30}')
    first = storage_manager.storage_snapshot(include_details=False)
    storage_manager.SETTINGS_FILE.write_text('{"auto_cleanup":true,"retention_days":7}')
    second = storage_manager.storage_snapshot(include_details=False)
    assert first['auto_cleanup'] is False and first['retention_days'] == 30
    assert second['auto_cleanup'] is True and second['retention_days'] == 7
    assert second['free_bytes'] > 0
    assert 'root_cleanup_targets' not in second and 'event_count' not in second


def test_batched_network_fields_preserve_addresses_and_escaped_values(monkeypatch):
    calls = []

    def read(*args, **kwargs):
        calls.append(args)
        return subprocess.CompletedProcess(args, 0,
            'GENERAL.HWADDR:02\\:11\\:22\\:33\\:44\\:55\n'
            'IP4.ADDRESS[1]:192.0.2.10/24\nIP4.ADDRESS[2]:192.0.2.11/24\nIP4.GATEWAY:\n', '')

    monkeypatch.setattr(network_manager, 'nmcli', read)
    values = network_manager._read_fields('device', ('GENERAL.HWADDR', 'IP4.ADDRESS', 'IP4.GATEWAY'), 'eth0')
    assert values == {'GENERAL.HWADDR': ['02:11:22:33:44:55'],
                      'IP4.ADDRESS': ['192.0.2.10/24', '192.0.2.11/24'], 'IP4.GATEWAY': []}
    assert len(calls) == 1 and calls[0][-3:] == ('device', 'show', 'eth0')


def test_vendor_nmcli_can_fall_back_to_individual_read_only_fields(monkeypatch):
    monkeypatch.setattr(network_manager, 'nmcli', lambda *a, **k: subprocess.CompletedProcess(a, 1, '', 'unsupported fields'))
    monkeypatch.setattr(network_manager, '_values', lambda scope, field, identifier: [field])
    assert network_manager._read_fields('connection', ('ipv4.method', 'ipv4.dns'), 'example') == {
        'ipv4.method': ['ipv4.method'], 'ipv4.dns': ['ipv4.dns']}


def test_network_snapshot_batches_reads_and_observes_same_uuid_changes(monkeypatch):
    uid = '11111111-1111-1111-1111-111111111111'
    address = ['192.0.2.10/24']
    calls = []
    monkeypatch.setattr(network_manager, 'command_path', lambda *_: '/isolated/nmcli')
    monkeypatch.setattr(network_manager, '_device_rows', lambda: [dict(device='eth0', type='ethernet', state='connected', connection='wired')])
    monkeypatch.setattr(network_manager, '_connection_rows', lambda: [dict(name='wired', uuid=uid, type='ethernet', device='eth0', autoconnect=True, active=True)])

    def read(*args, **kwargs):
        calls.append(args)
        assert args[:4] == ('-t', '-m', 'multiline', '-f')
        if 'device' in args:
            output = f'GENERAL.CON-UUID:{uid}\nGENERAL.HWADDR:02:11:22:33:44:55\nIP4.ADDRESS[1]:{address[0]}\nIP4.GATEWAY:192.0.2.1\nIP4.DNS[1]:192.0.2.53\n'
        else:
            output = f'ipv4.method:manual\nipv4.addresses:{address[0]}\nipv4.gateway:192.0.2.1\nipv4.dns:192.0.2.53\n'
        return subprocess.CompletedProcess(args, 0, output, '')

    monkeypatch.setattr(network_manager, 'nmcli', read)
    first = network_manager.network_snapshot()
    address[0] = '192.0.2.20/24'
    second = network_manager.network_snapshot()
    assert first['interfaces'][0]['addresses'] == ['192.0.2.10/24']
    assert second['interfaces'][0]['addresses'] == ['192.0.2.20/24']
    assert second['connections'][0]['addresses'] == ['192.0.2.20/24']
    assert second['interfaces'][0]['active_profile']['ipv4_method'] == 'manual'
    assert len(calls) == 4
