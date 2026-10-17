import json
import os
from pathlib import Path
from types import SimpleNamespace
import time

import pytest

from rk3588_runtime import device_info as device, fault_diagnostics as diagnostics, manager, performance, recovery_history as history


def write(root, name, content):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content)
    return path


@pytest.fixture
def local_device(tmp_path, monkeypatch):
    for module in (device, diagnostics, performance):
        monkeypatch.setattr(module, 'PROC', tmp_path / 'proc')
        monkeypatch.setattr(module, 'SYS', tmp_path / 'sys')
    monkeypatch.setattr(device, 'ETC', tmp_path / 'etc')
    monkeypatch.setattr(device, '_cached', None)
    monkeypatch.setattr(device, '_cached_at', 0)
    monkeypatch.setattr(device, '_cpu_previous', None)
    monkeypatch.setattr(device, '_cpu_usage', None)
    monkeypatch.setattr(manager, 'STATUS_FILE', tmp_path / 'manager.json')
    monkeypatch.setattr(history, 'boot_id', lambda: 'boot-test')
    write(device.PROC, 'sys/kernel/hostname', 'edge-test')
    write(device.PROC, 'sys/kernel/osrelease', '6.1-vendor')
    write(device.PROC, 'stat', 'cpu 100 0 0 100 0 0 0 0 80 0\n')
    write(device.PROC, 'loadavg', '0.10 0.20 0.30 1/100 200')
    write(device.PROC, 'uptime', '3600.5 1000')
    write(device.PROC, 'meminfo', 'MemTotal: 1000 kB\nMemAvailable: 600 kB\nMemFree: 20 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\n')
    write(device.PROC, 'mounts', '')
    write(device.ETC, 'os-release', 'PRETTY_NAME="Example Linux 12"\n')
    write(device.ETC, 'resolv.conf', 'nameserver 192.0.2.53\n')
    write(device.SYS, 'firmware/devicetree/base/model', 'Example RK3588 carrier')
    write(device.SYS, 'firmware/devicetree/base/compatible', 'example,board\x00rockchip,rk3588\x00')
    write(device.SYS, 'firmware/devicetree/base/serial-number', 'serial-test')
    commands = []
    def command(args):
        commands.append(args)
        if args[0] == 'ip':
            return '[]'
        if args[0] == 'timedatectl':
            return 'NTP=yes\nNTPSynchronized=no\n'
        return '\n\n'.join(f'Id={unit}\nLoadState=loaded\nActiveState=active\nSubState=running\nUnitFileState=enabled' for unit in device.SERVICES)
    monkeypatch.setattr(diagnostics, 'bounded_command', command)
    return commands


def test_overview_is_read_only_cached_and_independent_of_history(local_device, tmp_path):
    before = {path: path.read_bytes() for path in tmp_path.rglob('*') if path.is_file()}
    value = device.snapshot()
    assert value['system']['hostname'] == 'edge-test'
    assert value['system']['os_name'] == 'Example Linux 12'
    assert value['system']['soc'] == 'RK3588' and value['system']['serial'] == 'serial-test'
    assert value['system']['ntp_enabled'] is True and value['system']['ntp_synchronized'] is False
    assert value['memory']['usage_percent'] == 40
    assert value['diagnostics']['board']['model'] == 'Example RK3588 carrier'
    assert len(local_device) == 5
    value['system']['hostname'] = 'caller mutation'
    assert device.snapshot()['system']['hostname'] == 'edge-test' and len(local_device) == 5
    assert not history.HISTORY_FILE.exists()
    assert before == {path: path.read_bytes() for path in tmp_path.rglob('*') if path.is_file()}
    assert all(args[0] in ('ip', 'timedatectl', 'systemctl') and ('show' in args) for args in local_device)


def test_cpu_interval_excludes_double_counted_guest_time(local_device):
    assert device.cpu_usage() is None
    write(device.PROC, 'stat', 'cpu 150 0 0 150 0 0 0 0 90 0\n')
    assert device.cpu_usage() == 50
    write(device.PROC, 'stat', 'cpu 1 0 0 1 0 0 0 0 0 0\n')
    assert device.cpu_usage() is None


def test_missing_optional_interfaces_do_not_hide_other_information(local_device, monkeypatch):
    (device.PROC / 'stat').unlink()
    def unavailable(args):
        raise FileNotFoundError(args[0])
    monkeypatch.setattr(diagnostics, 'bounded_command', unavailable)
    value = device.snapshot()
    assert value['system']['hostname'] == 'edge-test'
    assert value['cpu']['architecture'] and value['cpu']['usage_sampling'] is False
    assert value['memory']['usage_percent'] == 40
    assert value['network']['interfaces'] == [] and value['temperatures'] == []
    assert value['services'][0]['active_state'] == 'unknown'
    assert value['system']['ntp_synchronized'] is None and value['errors']


def test_missing_memory_or_uptime_does_not_hide_hostname_and_network(local_device):
    (device.PROC / 'meminfo').unlink()
    (device.PROC / 'uptime').unlink()
    value = device.snapshot()
    assert value['memory'] == {} and value['system']['uptime_sec'] is None
    assert value['system']['hostname'] == 'edge-test' and value['system']['os_name']
    assert value['services'][0]['active_state'] == 'active'


def test_memory_fallback_is_labeled_and_clamped(local_device):
    write(device.PROC, 'meminfo', 'MemTotal: 1000 kB\nMemFree: 20 kB\nCached: 100 kB\nBuffers: 10 kB\nSwapTotal: 200 kB\nSwapFree: 50 kB\n')
    value = device.memory_info()
    assert value['available_estimated'] and value['available_bytes'] == 130 * 1024
    assert value['swap_used_bytes'] == 150 * 1024


def test_storage_deduplicates_mounts_excludes_network_and_follows_app_symlink(local_device, tmp_path, monkeypatch):
    root, data, bind, network = [tmp_path / name for name in ('root', 'data volume', 'bind', 'network')]
    for path in (root, data, bind, network):
        path.mkdir()
    (data / 'apps').mkdir()
    apps = tmp_path / 'apps'
    apps.symlink_to(data / 'apps')
    monkeypatch.setenv('APPS_ROOT', str(apps))
    escaped = str(data).replace(' ', r'\040')
    write(device.PROC, 'mounts', f'/dev/root {root} ext4 rw 0 0\n/dev/data {escaped} ext4 ro 0 0\n/dev/data {bind} ext4 rw 0 0\nserver:/share {network} nfs rw 0 0\n')
    real_stat = os.stat
    def stat(path, *args, **kwargs):
        if str(path) == str(network):
            pytest.fail('不能探测可能阻塞的网络挂载')
        original = real_stat(path, *args, **kwargs)
        if str(path) in (str(root), str(data), str(bind)):
            values = list(original)
            values[2] = 100 if str(path) == str(root) else 200
            return os.stat_result(values)
        return original
    monkeypatch.setattr(device.os, 'stat', stat)
    monkeypatch.setattr(device.os, 'statvfs', lambda path: SimpleNamespace(f_blocks=1000, f_frsize=1024,
                        f_bfree=400, f_bavail=300, f_flag=os.ST_RDONLY if str(path) == str(data) else 0))
    values = device.storage_info()
    assert len(values) == 2
    assert values[1]['mount'] == str(data) and values[1]['readonly'] and values[1]['application_data']
    assert values[0]['usage_percent'] == 60 and values[0]['available_bytes'] == 300 * 1024


def test_network_keeps_global_ip_and_routing_without_testing_reachability(local_device):
    (device.SYS / 'class/net/eth5/device').mkdir(parents=True)
    write(device.SYS, 'class/net/eth5/speed', '2500')
    values = device.network_info([
        {'ifname': 'lo'}, {'ifname': 'docker0'}, {'ifname': 'dummy0'},
        {'ifname': 'eth5', 'flags': ['UP', 'LOWER_UP'], 'address': '00:11:22:33:44:55',
         'addr_info': [{'local': '192.0.2.2', 'prefixlen': 24, 'scope': 'global'},
                       {'local': 'fe80::1', 'prefixlen': 64, 'scope': 'link'},
                       {'local': '2001:db8::1', 'prefixlen': 64, 'scope': 'global', 'flags': ['tentative']}]}],
        [{'dev': 'eth5', 'gateway': '192.0.2.1'}], [{'dev': 'eth5', 'gateway': '2001:db8::ff'}])
    assert len(values['interfaces']) == 1
    interface = values['interfaces'][0]
    assert interface['addresses'] == ['192.0.2.2/24'] and interface['speed_mbps'] == 2500
    assert interface['gateways'] == ['192.0.2.1', '2001:db8::ff'] and values['dns'] == ['192.0.2.53']


def test_one_bad_route_source_does_not_hide_addresses(local_device, monkeypatch):
    def command(args):
        if 'address' in args:
            return '[{"ifname":"eth0","flags":["UP"],"addr_info":[{"local":"192.0.2.2","prefixlen":24,"scope":"global"}]}]'
        return 'malformed'
    monkeypatch.setattr(diagnostics, 'bounded_command', command)
    assert device.snapshot()['network']['interfaces'][0]['addresses'] == ['192.0.2.2/24']


def test_temperature_groups_preserve_any_critical_alarm(local_device):
    for number, (kind, temp, trip) in enumerate([('bigcore0', 100000, 120000), ('bigcore1', 95000, 90000), ('gpu', 50000, None)]):
        zone = f'class/thermal/thermal_zone{number}/'
        write(device.SYS, zone + 'type', kind)
        write(device.SYS, zone + 'temp', str(temp))
        if trip:
            write(device.SYS, zone + 'trip_point_0_type', 'critical')
            write(device.SYS, zone + 'trip_point_0_temp', str(trip))
    values = device.temperature_info()
    cpu = next(item for item in values if item['label'] == 'CPU')
    assert cpu['celsius'] == 100 and cpu['critical_reached']
    assert len(values) == 2


def test_application_does_not_report_stale_or_disabled_supervisor_as_no_app(local_device):
    write(manager.STATUS_FILE.parent, manager.STATUS_FILE.name, json.dumps({'boot_id': 'old', 'updated_at': time.time(), 'current': {'app': 'old-app'}}))
    assert device.application_info()['state'] == 'unavailable'
    manager.STATUS_FILE.write_text(json.dumps({'boot_id': 'boot-test', 'updated_at': time.time(), 'phase': 'disabled'}))
    assert device.application_info()['state'] == 'disabled'
    manager.STATUS_FILE.write_text(json.dumps({'boot_id': 'boot-test', 'updated_at': time.time(), 'current': {'app': 'demo', 'status': 'running', 'config': 'live.json'}}))
    assert device.application_info()['name'] == 'demo'


def test_corrupt_diagnostics_do_not_hide_live_device_overview(local_device):
    diagnostics.STORE.parent.mkdir(parents=True)
    diagnostics.STORE.write_text('{broken')
    value = device.snapshot()
    assert value['diagnostics']['error']
    assert value['diagnostics']['board']['model'] == 'Example RK3588 carrier'
    assert value['system']['hostname'] == 'edge-test'


def test_live_counters_update_each_request_while_slow_queries_stay_cached(local_device):
    first = device.snapshot()
    assert first['cpu']['usage_percent'] is None
    write(device.PROC, 'stat', 'cpu 150 0 0 150 0 0 0 0 90 0\n')
    write(device.PROC, 'meminfo', 'MemTotal: 1000 kB\nMemAvailable: 300 kB\n')
    write(device.SYS, 'class/devfreq/vendor.gpu/cur_freq', '600000000')
    write(device.SYS, 'class/devfreq/vendor.gpu/load', '35@600000000Hz')
    second = device.snapshot()
    assert second['cpu']['usage_percent'] == 50
    assert second['memory']['usage_percent'] == 70
    assert second['performance']['devices'][0]['current_mhz'] == 600
    assert second['performance']['devices'][0]['usage_percent'] == 35
    assert len(local_device) == 5 and second['updated_at'] > first['updated_at']
