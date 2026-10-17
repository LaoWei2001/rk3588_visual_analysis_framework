"""Opt-in real daemon execution with disposable, fake network command backends."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'python'))
from rk3588_runtime import network_policy


@pytest.mark.skipif(os.environ.get('RK3588_SYSTEMD_TESTS') != '1', reason='requires local systemd opt-in')
@pytest.mark.parametrize('target', [{'host': '192.0.2.10', 'port': 443}, {'host': '192.0.2.10', 'method': 'ping'}])
def test_staged_network_recovery_runs_without_web_and_persists_before_action(tmp_path, target):
    installer = Path(__file__).resolve().parents[1] / 'install_runtime.py'
    stage = tmp_path / 'platform'
    subprocess.run([sys.executable, str(installer), '--stage', str(stage)], check=True, timeout=10)
    fake = tmp_path / 'commands'
    fake.mkdir()
    code = r'''#!/usr/bin/python3
import json,os,sys
from pathlib import Path
name=Path(sys.argv[0]).name
args=sys.argv[1:]
if name=='ip':
    if 'address' in args:
        print(json.dumps([{'ifname':'test0','flags':['LOWER_UP'],'addr_info':[{'scope':'global','local':'192.0.2.2','prefixlen':24}]}]))
    elif '-6' in args: print('[]')
    else: print(json.dumps([{'dev':'test0','gateway':'192.0.2.1','metric':100}]))
elif name=='ping': sys.exit(1)
elif name=='nmcli':
    if 'status' in args: print('test0:ethernet:connected')
    elif 'GENERAL.CON-UUID' in args: print('01234567-89ab-cdef-0123-456789abcdef')
    else:
        state=json.loads(Path(os.environ['NETWORK_RECOVERY_STATE_FILE']).read_text())
        assert state['stage']==1 and len(state['actions'])==1
        Path(os.environ['NETWORK_TEST_ACTION_FILE']).write_text(json.dumps(args))
else: raise RuntimeError('Unexpected systemctl action')
'''
    for name in ('ip', 'nmcli', 'ping', 'systemctl'):
        (fake / name).write_text(code)
        (fake / name).chmod(0o755)
    config = network_policy.validate({'enabled': True, 'interface': 'test0',
                                      'targets': [target]})
    config_file = tmp_path / 'policy.json'
    config_file.write_text(json.dumps(config))
    state_file = tmp_path / 'state.json'
    state_file.write_text(json.dumps({
        'boot_id': Path('/proc/sys/kernel/random/boot_id').read_text().strip(),
        'policy_revision': network_policy.revision(config), 'interface': 'test0', 'kind': 'ethernet',
        'armed': True, 'known_gateway': '192.0.2.1', 'stage': 0, 'fault_since': time.monotonic() - 400,
    }))
    environment = {'PYTHONPATH': str(stage / 'usr/lib/vision-analysis-platform/python'),
                   'PATH': str(fake) + ':/usr/bin:/bin', 'NETWORK_RECOVERY_POLICY_FILE': str(config_file),
                   'NETWORK_RECOVERY_STATE_FILE': str(state_file),
                   'REBOOT_COORDINATOR_STATE_FILE': str(tmp_path / 'reboot/state.json'),
                   'KERNEL_RECOVERY_STATE_FILE': str(tmp_path / 'kernel-state.json'),
                   'RECOVERY_HISTORY_FILE': str(tmp_path / 'history/events.json'),
                   'NETWORK_RECOVERY_STATUS_FILE': str(tmp_path / 'status.json'),
                   'NETWORK_TRANSACTION_DIR': str(tmp_path / 'transactions'),
                   'NETWORK_TEST_ACTION_FILE': str(tmp_path / 'action.json')}
    unit = 'rk3588-network-recovery-isolated-test.service'
    command = ['systemd-run', '--quiet', '--collect', '--wait', '--pipe', '--unit=' + unit,
               '--service-type=exec', '--property=TimeoutStartSec=30']
    command += [f'--setenv={key}={value}' for key, value in environment.items()]
    command += ['/usr/bin/python3', '-m', 'rk3588_runtime.network_recovery', '--once']
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=35)
        assert result.returncode == 0, result.stdout + result.stderr
        assert json.loads((tmp_path / 'action.json').read_text()) == [
            '--wait', '15', 'connection', 'up', 'uuid', '01234567-89ab-cdef-0123-456789abcdef', 'ifname', 'test0']
        assert json.loads((tmp_path / 'status.json').read_text())['phase'] == 'recovering'
        assert 'web_console' not in environment['PYTHONPATH']
    finally:
        subprocess.run(['systemctl', 'stop', unit], capture_output=True, timeout=10, check=False)
