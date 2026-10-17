import asyncio

import pytest
from fastapi import HTTPException

from routers import recovery


@pytest.fixture
def policy_file(tmp_path, monkeypatch):
    monkeypatch.setattr(recovery.policy, 'POLICY_FILE', tmp_path / 'recovery.json')
    monkeypatch.setattr(recovery.management, 'snapshot', lambda: {
        'policy': recovery.policy.read(), 'revision': recovery.policy.revision(recovery.policy.read())})
    return recovery.policy


def test_web_saves_system_policy_with_conflict_detection(policy_file):
    original = policy_file.read()
    revision = policy_file.revision(original)
    result = asyncio.run(recovery.save_recovery({
        'policy': {**original, 'task_timeout_sec': 180}, 'revision': revision,
    }))
    assert result['policy']['task_timeout_sec'] == 180
    with pytest.raises(HTTPException) as error:
        asyncio.run(recovery.save_recovery({'policy': original, 'revision': revision}))
    assert error.value.status_code == 409
    assert policy_file.read()['task_timeout_sec'] == 180


def test_rejects_unknown_fields_and_unbounded_reboot_policy(policy_file):
    original = policy_file.read()
    revision = policy_file.revision(original)
    for value in ({**original, 'max_reboots_per_day': 10000},
                  {**original, 'restart_delay_sec': 0},
                  {**original, 'arbitrary_command': 'reboot'}):
        with pytest.raises(HTTPException) as error:
            asyncio.run(recovery.save_recovery({'policy': value, 'revision': revision}))
        assert error.value.status_code == 400
    assert not policy_file.POLICY_FILE.exists()


def test_service_control_is_whitelisted(monkeypatch):
    def unexpected(*args, **kwargs):
        pytest.fail('unknown actions must never execute a system command')
    monkeypatch.setattr(recovery.management.subprocess, 'run', unexpected)
    with pytest.raises(HTTPException) as error:
        asyncio.run(recovery.control_agent('reboot'))
    assert error.value.status_code == 400


def test_recovery_endpoints_require_login():
    from main import app
    async def run():
        for method, path in [('GET', '/api/system/recovery'), ('PUT', '/api/system/recovery'),
                             ('GET', '/api/system/recovery/history'),
                             ('GET', '/api/system/device-info'),
                             ('PUT', '/api/system/device-info/performance'),
                             ('POST', '/api/system/device-info/performance/reset'),
                             ('PUT', '/api/system/recovery/system'),
                             ('PUT', '/api/system/recovery/application'), ('POST', '/api/system/recovery/application/apply'),
                             ('GET', '/api/system/recovery/network'), ('PUT', '/api/system/recovery/network'),
                             ('POST', '/api/system/recovery/agent/start'), ('POST', '/api/system/recovery/apply')]:
            messages = []
            scope = {'type': 'http', 'asgi': {'version': '3.0'}, 'http_version': '1.1',
                     'scheme': 'http', 'method': method, 'path': path, 'raw_path': path.encode(),
                     'root_path': '', 'query_string': b'', 'headers': [],
                     'server': ('device', 80), 'client': ('127.0.0.1', 12345)}
            async def receive():
                return {'type': 'http.request', 'body': b'', 'more_body': False}
            async def send(message):
                messages.append(message)
            await app(scope, receive, send)
            assert any(message.get('status') == 401 for message in messages)
    asyncio.run(run())


def test_history_endpoint_reads_without_system_commands(monkeypatch):
    history = recovery.management.recovery_history
    history.record('system', 'maintenance', '功能更新', '重启管理服务', fault_recovery=False)
    monkeypatch.setattr(recovery.management.subprocess, 'run', lambda *args, **kwargs:
                        pytest.fail('读取历史不能执行服务控制或硬件探测'))
    value = asyncio.run(recovery.get_recovery_history())
    assert value['keep_operations'] == 20
    assert value['events'][0]['reason'] == '功能更新'
    assert value['events'][0]['fault_recovery'] is False


def test_device_info_endpoint_uses_read_only_overview(monkeypatch):
    monkeypatch.setattr(recovery.device_info, 'snapshot', lambda: {'system': {'hostname': 'edge-test'}})
    assert asyncio.run(recovery.get_device_info()) == {'system': {'hostname': 'edge-test'}}


def test_application_settings_only_change_application_fields_without_restarting(policy_file, monkeypatch):
    original = policy_file.write({**policy_file.DEFAULTS, 'kernel_reboot_enabled': True, 'kernel_hang_sec': 600})
    value = {key: original[key] for key in policy_file.APP_KEYS}
    value.update(app_restart_enabled=True, app_stall_enabled=True, task_timeout_sec=180)
    monkeypatch.setattr(recovery.management.pm, 'start_app', lambda *args: pytest.fail('saving must not restart the app'))
    result = asyncio.run(recovery.save_application_recovery({'policy': value, 'revision': policy_file.revision(original)}))
    assert result['policy'] == {**original, **value}
    assert result['policy']['kernel_hang_sec'] == 600 and result['policy']['manager_enabled']
    with pytest.raises(HTTPException) as conflict:
        asyncio.run(recovery.save_application_recovery({'policy': value, 'revision': policy_file.revision(original)}))
    assert conflict.value.status_code == 409


@pytest.mark.parametrize('change', [{'kernel_reboot_enabled': True}, {'task_timeout_sec': 1},
                                  {'app_stall_enabled': True, 'app_restart_enabled': False}])
def test_application_settings_reject_system_fields_invalid_limits_and_incomplete_recovery(policy_file, change):
    original = policy_file.read()
    value = {key: original[key] for key in policy_file.APP_KEYS}
    value.update(change)
    with pytest.raises(HTTPException) as error:
        asyncio.run(recovery.save_application_recovery({'policy': value, 'revision': policy_file.revision(original)}))
    assert error.value.status_code == 400
    assert not policy_file.POLICY_FILE.exists()


def test_application_apply_checks_saved_revision_and_current_program(policy_file, monkeypatch):
    from contextlib import nullcontext
    from rk3588_runtime import background_services
    monkeypatch.setattr(recovery.management.pm, 'runtime_lock', nullcontext)
    monkeypatch.setattr(recovery.management.pm, 'get_running_app_context', lambda: {'app': 'demo', 'config': 'production.json'})
    started = []
    monkeypatch.setattr(recovery.management.pm, 'start_app', lambda *args: started.append(args))
    monkeypatch.setattr(background_services, 'sync_services_for_running_app', lambda: None)
    fingerprint = policy_file.revision(policy_file.read())
    for value in ({'revision': 'stale', 'app': 'demo'}, {'revision': fingerprint, 'app': 'previous-app'}):
        with pytest.raises(HTTPException) as error:
            asyncio.run(recovery.apply_application_recovery(value))
        assert error.value.status_code == 409 and not started
    asyncio.run(recovery.apply_application_recovery({'revision': fingerprint, 'app': 'demo'}))
    assert started == [('demo', 'production.json')]


def test_network_recovery_write_requires_network_administrator():
    route = next(route for route in recovery.router.routes
                 if route.path == '/system/recovery/network' and 'PUT' in route.methods)
    assert any(dependency.call is recovery.require_network_admin for dependency in route.dependant.dependencies)


def test_system_switch_write_requires_administrator():
    route = next(route for route in recovery.router.routes
                 if route.path == '/system/recovery/system' and 'PUT' in route.methods)
    assert any(dependency.call is recovery.require_network_admin for dependency in route.dependant.dependencies)


def test_system_switch_endpoint_forwards_only_narrow_policy_and_revision(monkeypatch):
    calls = []
    monkeypatch.setattr(recovery.management, 'save_system_policy',
                        lambda value, revision: calls.append((value, revision)) or {'ok': True})
    value = {'hardware_enabled': False, 'panic_enabled': True,
             'watchdog_timeout_sec': 120, 'reboot_timeout_sec': 180, 'panic_timeout_sec': 25}
    assert asyncio.run(recovery.save_system_recovery({'policy': value, 'revision': 'saved'}, None)) == {'ok': True}
    assert calls == [(value, 'saved')]
    with pytest.raises(HTTPException) as error:
        asyncio.run(recovery.save_system_recovery({'policy': value}, None))
    assert error.value.status_code == 400 and len(calls) == 1


@pytest.mark.parametrize('target', [{'host': '192.0.2.10', 'port': 443}, {'host': '192.0.2.10', 'method': 'ping'}])
def test_network_policy_http_validation_and_cas(tmp_path, monkeypatch, target):
    from rk3588_runtime import network_policy
    monkeypatch.setattr(network_policy, 'POLICY_FILE', tmp_path / 'network-policy.json')
    monkeypatch.setattr(recovery.management, 'network_snapshot', lambda: {'policy': network_policy.read()})
    monkeypatch.setattr(recovery.management, 'unit_status', lambda unit: {'installed': True, 'active': True, 'enabled': True})
    original = network_policy.read()
    fingerprint = network_policy.revision(original)
    result = asyncio.run(recovery.save_network_recovery({'policy': original, 'revision': fingerprint}, None))
    assert not result['policy']['enabled']
    value = {**original, 'targets': [target]}
    saved = asyncio.run(recovery.save_network_recovery({'policy': value, 'revision': fingerprint}, None))
    assert saved['policy']['targets'] == [target]
    with pytest.raises(HTTPException) as conflict:
        asyncio.run(recovery.save_network_recovery({'policy': original, 'revision': fingerprint}, None))
    assert conflict.value.status_code == 409
    with pytest.raises(HTTPException) as invalid:
        asyncio.run(recovery.save_network_recovery({'policy': {**value, 'interface': 'lo'}, 'revision': fingerprint}, None))
    assert invalid.value.status_code == 400


def test_performance_mutations_require_admin_and_map_errors(monkeypatch):
    for path in ('/system/device-info/performance', '/system/device-info/performance/reset'):
        route = next(route for route in recovery.router.routes if route.path == path)
        assert any(dep.call is recovery.require_network_admin for dep in route.dependant.dependencies)
    calls = []
    monkeypatch.setattr(recovery.performance, 'save', lambda *args: calls.append(args) or {'applied': True})
    payload = {'revision': 'example', 'limits': {'CPU:0': 408}}
    assert asyncio.run(recovery.save_performance(payload)) == {'applied': True}
    assert calls == [(payload,)]
    asyncio.run(recovery.reset_performance({'revision': 'example'}))
    assert calls[-1] == ({'revision': 'example'}, True)
    for exc, status in [(ValueError('invalid'), 400), (recovery.policy.PolicyConflict('changed'), 409),
                        (OSError('kernel rejected'), 503)]:
        def fail(*args):
            raise exc
        monkeypatch.setattr(recovery.performance, 'save', fail)
        with pytest.raises(HTTPException) as result:
            asyncio.run(recovery.save_performance(payload))
        assert result.value.status_code == status
