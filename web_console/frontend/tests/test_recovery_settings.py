"""Recovery form regression checks with isolated policies and no device operations."""

from copy import deepcopy
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
from pathlib import Path
import threading
import time
from urllib.parse import urlsplit

import pytest

playwright = pytest.importorskip('playwright.sync_api')
DIST = Path(__file__).resolve().parents[1] / 'dist'


@pytest.fixture
def recovery_ui():
    if not (DIST / 'index.html').is_file():
        pytest.skip('Build the frontend first')
    system_policy = dict(manager_enabled=True, app_restart_enabled=False, app_stall_enabled=True,
                         restart_delay_sec=17, task_timeout_sec=150, startup_timeout_sec=400,
                         kernel_reboot_enabled=True, kernel_hang_sec=240,
                         reboot_cooldown_sec=2400, max_reboots_per_day=4)
    network_policy = dict(enabled=False, interface='', targets=[], failure_timeout_sec=180,
                          recovery_wait_sec=60, action_cooldown_sec=300, allow_manager_restart=True,
                          allow_reboot=False, reboot_after_sec=900, reboot_cooldown_sec=3600,
                          max_reboots_per_day=2)
    system = dict(policy=system_policy, revision='system-1', policy_error=None,
                  system=dict(policy=dict(hardware_enabled=True, panic_enabled=True,
                                          watchdog_timeout_sec=89, reboot_timeout_sec=60, panic_timeout_sec=10), revision='switches-1', error=None,
                              can_enable_hardware=True, can_set_panic=True, hardware_unavailable_reason=None,
                              hardware_pending=False, hardware_apply_error=None, panic_in_sync=True),
                  agent=dict(installed=True, active=True, enabled=True, state='active', heartbeat_fresh=True, error=None),
                  monitor=dict(installed=True, active=True, error=None),
                  hardware=dict(enabled=True, device='/dev/watchdog1', driver='dw_wdt',
                                runtime_timeout='1min 29s', reboot_timeout='1min', panic_timeout_sec=10,
                                verification_state='passed', error=None),
                  runtime=dict(phase='idle', error=None, retry_in_sec=0, updated_at=1),
                  current=None, boot_target=None, application_pending=False, events=[])
    network = dict(policy=network_policy, revision='network-1', policy_error=None,
                   service=dict(installed=True, active=True, enabled=True, heartbeat_fresh=True, error=None),
                   interfaces=[dict(name='eth0', type='ethernet', state='connected')],
                   interface_error=None, policy_pending=False, runtime=None, events=[])
    snapshots = {'/api/system/recovery': system, '/api/system/recovery/network': network,
                 '/api/system/device-info': device_overview(),
                 '/api/system/recovery/history': {'events': [], 'keep_per_category': 20, 'keep_operations': 20,
                                                  'error': None, 'observer': {'updated_at': time.time()}}}
    writes = []

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(DIST), **kwargs)

        def do_GET(self):
            if self.path.startswith(('/recovery', '/system-settings')):
                self.path = '/index.html'
            super().do_GET()

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with playwright.sync_playwright() as pw:
            browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                         headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
            context = browser.new_context(viewport={'width': 1366, 'height': 1100})
            context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'isolated-test',username:'tester'},version:0}))")
            page = context.new_page()
            errors = []
            page.on('pageerror', lambda error: errors.append(str(error)))

            def api(route):
                request = route.request
                path = urlsplit(request.url).path
                application = path.startswith('/api/system/recovery/application')
                system_switches = path == '/api/system/recovery/system'
                key = '/api/system/recovery' if application or system_switches else path
                assert key in snapshots, path
                if request.method == 'PUT':
                    payload = request.post_data_json
                    assert payload['revision'] == (snapshots[key]['system']['revision'] if system_switches else snapshots[key]['revision'])
                    writes.append((path, deepcopy(payload)))
                    if system_switches:
                        assert set(payload['policy']) == {'hardware_enabled', 'panic_enabled', 'watchdog_timeout_sec',
                                                         'reboot_timeout_sec', 'panic_timeout_sec'}
                        timeouts_changed = any(payload['policy'][timeout_key] != snapshots[key]['system']['policy'][timeout_key]
                                               for timeout_key in ('watchdog_timeout_sec', 'reboot_timeout_sec'))
                        snapshots[key]['system']['policy'] = payload['policy']
                        snapshots[key]['system']['configured'] = True
                        snapshots[key]['system']['revision'] += '-saved'
                        snapshots[key]['system']['hardware_pending'] = (
                            payload['policy']['hardware_enabled'] != snapshots[key]['hardware']['enabled']
                            or payload['policy']['hardware_enabled'] and (
                                timeouts_changed or snapshots[key]['system']['hardware_pending']))
                        snapshots[key]['hardware']['panic_timeout_sec'] = payload['policy']['panic_timeout_sec'] if payload['policy']['panic_enabled'] else 0
                        snapshots[key]['system']['panic_in_sync'] = True
                    elif application:
                        snapshots[key]['policy'].update(payload['policy'])
                        snapshots[key]['application_pending'] = snapshots[key]['current'] is not None
                    else:
                        snapshots[key]['policy'] = payload['policy']
                    if not system_switches:
                        snapshots[key]['revision'] += '-saved'
                elif request.method == 'POST':
                    assert path == '/api/system/recovery/application/apply'
                    payload = request.post_data_json
                    assert payload == {'revision': snapshots[key]['revision'], 'app': snapshots[key]['current']['app']}
                    writes.append((path, deepcopy(payload)))
                    snapshots[key]['application_pending'] = False
                    snapshots[key]['current']['restart_enabled'] = snapshots[key]['policy']['app_restart_enabled']
                    snapshots[key]['current']['watchdog_active'] = snapshots[key]['policy']['app_stall_enabled']
                else:
                    assert request.method == 'GET', request.method
                if key == '/api/system/device-info':
                    snapshots[key]['updated_at'] = time.time()
                route.fulfill(status=200, content_type='application/json', body=json.dumps(snapshots[key]))

            page.route('**/api/**', api)
            yield page, f'http://127.0.0.1:{server.server_port}', snapshots, writes
            assert not errors, errors
            browser.close()
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=5)


def visible_card(page):
    return page.locator('.recovery-settings:visible')


def operation_row(key, category, kind, fault=False):
    return dict(id=key, time=100, updated_at=101, category=category, kind=kind,
                reason=f'{key} 事件事由', action=f'{key} 执行方式', detail='操作已执行', result='performed',
                source='记录服务', evidence='原始记录依据', time_basis='发现时间', boot_id='boot', fault_recovery=fault)


def test_operations_module_filters_refreshes_and_preserves_settings_drafts(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    history = snapshots['/api/system/recovery/history']
    history['events'] = [operation_row('routine-system', 'system', 'service_replaced'),
                         operation_row('routine-network', 'network', 'manual_network_config'),
                         operation_row('routine-app', 'application', 'manual_stop'),
                         operation_row('fault-system', 'system', 'system_boot', True),
                         operation_row('fault-network', 'network', 'reconnect', True),
                         operation_row('fault-app', 'application', 'service_failure', True)]
    page.goto(url + '/recovery/operations')
    card = visible_card(page)
    playwright.expect(card.locator('h4')).to_have_text('运行与操作记录')
    playwright.expect(card.locator('.operation-history li')).to_have_count(3)
    playwright.expect(card.locator('li').filter(has_text='fault-system')).to_have_count(0)
    card.get_by_label('记录类别').select_option('network')
    playwright.expect(card.locator('.operation-history li')).to_have_count(1)
    playwright.expect(card.locator('li').filter(has_text='routine-network')).to_be_visible()
    card.get_by_label('记录类别').select_option('all')
    history['events'].insert(0, operation_row('new-operation', 'system', 'maintenance'))
    playwright.expect(card.locator('.operation-history li')).to_have_count(4, timeout=7500)
    card.get_by_text('记录依据', exact=True).first.click()
    for width in (1366, 390):
        page.set_viewport_size({'width': width, 'height': 1100})
        assert card.evaluate('(e) => e.scrollWidth <= e.clientWidth + 1')
    page.get_by_role('button', name='系统故障恢复', exact=False).click()
    control = visible_card(page).get_by_label('卡死复位等待时间（秒）', exact=True)
    control.fill('123')
    page.get_by_role('button', name='运行与操作记录', exact=False).click()
    playwright.expect(visible_card(page).locator('h4')).to_have_text('运行与操作记录')
    page.get_by_role('button', name='系统故障恢复', exact=False).click()
    playwright.expect(control).to_have_value('123')
    assert not writes


def test_operations_read_failure_retains_last_records(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    snapshots['/api/system/recovery/history']['events'] = [operation_row('retained-operation', 'system', 'maintenance')]
    page.goto(url + '/recovery/operations')
    card = visible_card(page)
    playwright.expect(card.locator('.operation-history li')).to_have_count(1)
    page.route('**/api/system/recovery/history', lambda route: route.fulfill(
        status=503, content_type='application/json', body=json.dumps({'detail': '记录暂不可用'})))
    playwright.expect(card.get_by_text('记录读取失败：记录暂不可用', exact=False)).to_be_visible(timeout=7500)
    playwright.expect(card.locator('.operation-history li').filter(has_text='retained-operation')).to_be_visible()
    assert not writes


def test_normal_reboot_is_one_operation_with_expandable_stages(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    reboot = operation_row('normal-reboot', 'system', 'system_boot')
    reboot.update(reason='到达每日定时重启时间', source='每日定时重启',
                  detail='已确认请求后设备重新启动，本次正常重启已完成。', stages=[
        dict(id='request', kind='board_reboot_request', time=100, action='systemd 定时任务请求整机重启', source='每日定时重启'),
        dict(id='shutdown', kind='system_shutdown', time=101, action='系统正在结束服务并准备关机或重启', source='系统关机通知'),
        dict(id='boot', kind='system_boot', time=110, action='设备启动确认', source='开机恢复检查'),
    ])
    snapshots['/api/system/recovery/history']['events'] = [reboot]
    page.goto(url + '/recovery/operations')
    card = visible_card(page)
    playwright.expect(card.locator('.operation-history li')).to_have_count(1)
    playwright.expect(card.get_by_text('已确认请求后设备重新启动，本次正常重启已完成。', exact=False)).to_be_visible()
    card.get_by_text('重启过程（3 个阶段）', exact=True).click()
    playwright.expect(card.locator('.recovery-history-stage')).to_have_count(3)
    for stage in ('发起重启', '进入正常关机流程', '设备已重新启动'):
        playwright.expect(card.locator('.recovery-history-stages').get_by_text(stage, exact=False)).to_be_visible()
    for width in (1366, 390):
        page.set_viewport_size({'width': width, 'height': 1100})
        assert card.evaluate('(e) => e.scrollWidth <= e.clientWidth + 1')
    assert not writes


def test_history_displays_inference_boundaries_without_repeated_device_info(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    snapshots['/api/system/recovery']['history'] = {
        'keep_per_category': 20, 'error': None, 'observer': {'updated_at': time.time()},
        'diagnostics': {'board': {'model': 'Example RK3588 carrier B', 'compatible': 'example,board,rockchip,rk3588',
                                  'kernel': '6.1-vendor', 'limitations': ['看门狗未提供 bootstatus']},
                        'sample_time': time.time(), 'errors': [], 'error': None},
        'events': [{'id': 'test-boot', 'time': time.time(), 'updated_at': time.time(), 'category': 'system',
                    'kind': 'system_boot', 'reason': '异常重启前发现 NPU 超时，与重启的因果关系待确认',
                    'action': '整机重新启动，复位方式未确认', 'result': 'performed', 'detail': '仅确认设备重新启动',
                    'source': '开机恢复检查', 'evidence': 'RKNPU: job timeout', 'time_basis': '发现时间', 'boot_id': 'test',
                    'diagnosis': {'confidence': 'probable', 'findings': [{'code': 'npu', 'summary': 'NPU 驱动或任务异常'}],
                                  'limitations': ['日志异常不直接证明重启根因']}}]}
    page.goto(url + '/recovery/system')
    panel = page.locator('.recovery-history:visible')
    panel.locator('> summary').click()
    playwright.expect(panel.get_by_text('综合迹象推测', exact=False)).to_be_visible()
    playwright.expect(panel.get_by_text('日志异常不直接证明重启根因')).to_be_visible()
    panel.get_by_text('判断依据', exact=True).click()
    playwright.expect(panel.get_by_text('RKNPU: job timeout', exact=True)).to_be_visible()
    for category, heading in [('network', '网络故障恢复'), ('application', '程序故障恢复'), ('system', '系统故障恢复')]:
        page.get_by_role('button', name=heading, exact=False).click()
        playwright.expect(visible_card(page).locator('h4')).to_have_text(heading)
        playwright.expect(page.get_by_text('当前板卡与取证能力', exact=True)).to_have_count(0)
        playwright.expect(page.locator('.device-information')).to_have_count(0)
    assert not writes


@pytest.mark.parametrize('confidence,label', [
    ('probable', '有依据的推测'), ('confirmed', '硬件证据确认'), ('unknown', '复位来源证据不足'),
])
def test_boot_history_separates_confirmed_fault_from_reset_method(recovery_ui, confidence, label):
    page, url, snapshots, writes = recovery_ui
    snapshots['/api/system/recovery']['history'] = {
        'keep_per_category': 20, 'error': None, 'observer': {'updated_at': time.time()},
        'events': [{'id': 'test-boot', 'time': time.time(), 'updated_at': time.time(), 'category': 'system',
                    'kind': 'system_boot', 'reason': '人为注入 HARDLOCKUP，日志记录 8 次',
                    'action': '旧的笼统恢复方式', 'result': 'performed', 'detail': '设备已重新启动',
                    'source': '开机恢复检查', 'evidence': 'lkdtm: Performing direct entry HARDLOCKUP',
                    'time_basis': '发现时间', 'boot_id': 'test',
                    'diagnosis': {'confidence': 'confirmed', 'findings': [], 'limitations': [],
                                  'recovery': {'confidence': confidence, 'method': '硬件看门狗超时复位',
                                               'basis': ['故障前 PID 1 持有硬件看门狗，超时 11 秒'],
                                               'limitations': ['看门狗未提供 bootstatus']}}}]}
    page.goto(url + '/recovery/system')
    panel = page.locator('.recovery-history:visible')
    panel.locator('> summary').click()
    playwright.expect(panel.get_by_text('故障判断：触发证据确认', exact=True)).to_be_visible()
    playwright.expect(panel.get_by_text('恢复判断：' + label, exact=True)).to_be_visible()
    playwright.expect(panel.get_by_text('依据：故障前 PID 1 持有硬件看门狗，超时 11 秒', exact=True)).to_be_visible()
    playwright.expect(panel.get_by_text('限制：看门狗未提供 bootstatus', exact=True)).to_be_visible()
    playwright.expect(panel.get_by_text('旧的笼统恢复方式', exact=False)).to_have_count(0)
    assert not writes


def device_diagnostics():
    return {'board': {'model': 'Example RK3588 carrier B', 'compatible': 'example,long-carrier-board-compatible,rockchip,rk3588',
                      'kernel': '6.1-vendor', 'limitations': ['看门狗未提供 bootstatus'],
                      'watchdogs': [{'device': 'watchdog1', 'identity': 'dw_wdt'}], 'reset_sources': [],
                      'pstore': True, 'psi': {'cpu': False, 'memory': False, 'io': False}},
            'sample_time': time.time(), 'errors': [], 'error': None}


def device_overview():
    return {'updated_at': time.time(), 'performance': performance_snapshot(),
            'system': {'hostname': 'edge-rk3588', 'os_name': 'Example Linux 12', 'soc': 'RK3588', 'serial': 'DEVICE-001',
                       'kernel': '6.1-vendor', 'uptime_sec': 3700, 'boot_time': time.time() - 3700,
                       'current_time': '2026-10-16T10:00:00+08:00', 'timezone': 'Asia/Shanghai',
                       'application_directory': '/opt/ai_apps', 'ntp_enabled': True, 'ntp_synchronized': True},
            'cpu': {'architecture': 'aarch64', 'cores': 8, 'max_mhz': 2400, 'usage_percent': 12.5, 'load_average': [0.1, 0.2, 0.3]},
            'memory': {'total_bytes': 8 * 1024 ** 3, 'available_bytes': 6 * 1024 ** 3, 'used_bytes': 2 * 1024 ** 3,
                       'usage_percent': 25, 'swap_total_bytes': 0, 'swap_used_bytes': 0},
            'storage': [{'mount': '/', 'source': '/dev/root', 'filesystem': 'ext4', 'total_bytes': 64 * 1024 ** 3,
                         'used_bytes': 20 * 1024 ** 3, 'available_bytes': 40 * 1024 ** 3, 'usage_percent': 31.25,
                         'readonly': False, 'application_data': True}],
            'network': {'interfaces': [{'name': 'eth0', 'kind': '网卡', 'link_up': True, 'mac': '00:11:22:33:44:55',
                                       'addresses': ['192.0.2.10/24'], 'speed_mbps': 1000, 'gateways': ['192.0.2.1']}],
                        'dns': ['192.0.2.53']},
            'temperatures': [{'label': 'CPU', 'celsius': 48.5, 'critical_celsius': 100}],
            'accelerators': [{'kind': 'NPU', 'registered': True, 'driver': 'RKNPU', 'current_mhz': 800}],
            'services': [{'unit': 'rk3588-console.service', 'label': 'Web 控制台', 'load_state': 'loaded',
                          'active_state': 'active', 'sub_state': 'running', 'autostart': True},
                         {'unit': 'ota_agent.service', 'label': '升级服务', 'load_state': 'not-found',
                          'active_state': 'inactive', 'sub_state': 'dead', 'autostart': False}],
            'application': {'state': 'running', 'name': 'demo', 'config': 'live.json'},
            'diagnostics': device_diagnostics(), 'observer': {'updated_at': time.time()}, 'errors': []}


def test_device_info_is_centralized_in_system_settings_and_read_only(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    requests = []
    page.on('request', lambda request: requests.append(request) if '/api/' in request.url else None)
    page.goto(url + '/system-settings?section=device')
    card = visible_card(page)
    playwright.expect(card.locator('h4')).to_have_text('设备基本信息')
    playwright.expect(page.get_by_role('button', name='设备基本信息', exact=False)).to_have_attribute('aria-current', 'page')
    for text in ('Example RK3588 carrier B', '6.1-vendor', 'edge-rk3588'):
        playwright.expect(card.get_by_text(text, exact=True)).to_be_visible()
    for heading in ('资源与温度', '存储空间', '网络概况', '关键服务状态'):
        playwright.expect(card.get_by_role('heading', name=heading, exact=True)).to_have_count(0)
    playwright.expect(card.locator('.performance-monitor')).to_have_count(0)
    playwright.expect(card.locator('.device-information-temperatures')).to_have_count(0)
    playwright.expect(card.get_by_text('性能与温控设置', exact=True)).to_have_count(0)
    playwright.expect(card.get_by_text('192.0.2.10/24', exact=True)).to_have_count(0)
    card.get_by_text('故障取证能力', exact=True).click()
    playwright.expect(card.get_by_text('看门狗未提供 bootstatus', exact=True)).to_be_visible()
    playwright.expect(card.get_by_text('watchdog1：dw_wdt', exact=False)).to_be_visible()
    nav = page.get_by_role('navigation', name='系统设置分类')
    nav.get_by_role('button', name='性能', exact=False).click()
    playwright.expect(page).to_have_url(re.compile(r'section=performance'))
    card = visible_card(page)
    playwright.expect(card.locator('h4')).to_have_text('性能')
    playwright.expect(card.locator('.performance-monitor')).to_be_visible()
    playwright.expect(card.get_by_text('edge-rk3588', exact=True)).to_have_count(0)
    playwright.expect(card.get_by_text('故障取证能力', exact=True)).to_have_count(0)
    card.get_by_text('性能与温控设置', exact=True).click()
    playwright.expect(card.get_by_text('临界温度可能触发关机或重启。', exact=True)).to_be_visible()
    for width in (1366, 390):
        page.set_viewport_size({'width': width, 'height': 1100})
        assert card.evaluate('e => e.scrollWidth <= e.clientWidth + 1')
    snapshots['/api/system/device-info']['diagnostics']['board']['model'] = 'Updated RK3588 carrier'
    nav.get_by_role('button', name='设备基本信息', exact=False).click()
    card = visible_card(page)
    playwright.expect(card.get_by_text('Updated RK3588 carrier', exact=True)).to_be_visible()
    playwright.expect(card.locator('.performance-monitor')).to_have_count(0)
    count = len(requests)
    page.wait_for_timeout(1300)
    # Device information now also polls every second; the unmounted performance
    # view must not leave a second poller running.
    assert 1 <= len(requests) - count <= 2
    assert requests and all(request.method == 'GET' and urlsplit(request.url).path == '/api/system/device-info' for request in requests)
    assert not writes


def test_device_info_refresh_failure_preserves_previous_information(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    snapshots['/api/system/device-info']['diagnostics'] = device_diagnostics()
    page.goto(url + '/system-settings?section=device')
    card = visible_card(page)
    playwright.expect(card.get_by_text('Example RK3588 carrier B', exact=True)).to_be_visible()
    page.route('**/api/system/device-info', lambda route: route.fulfill(
        status=503, content_type='application/json', body=json.dumps({'detail': '设备信息暂不可用'})))
    playwright.expect(card.get_by_role('button', name='刷新设备信息', exact=True)).to_have_count(0)
    page.evaluate("document.dispatchEvent(new Event('visibilitychange'))")
    playwright.expect(card.get_by_text('设备信息读取失败：设备信息暂不可用', exact=False)).to_be_visible()
    playwright.expect(card.get_by_text('Example RK3588 carrier B', exact=True)).to_be_visible()
    assert not writes


def test_device_overview_handles_missing_metrics_without_claiming_zero_usage(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    value = snapshots['/api/system/device-info']
    value.update(cpu={}, memory={}, storage=[], temperatures=[], accelerators=[], services=[], performance=None,
                 network={'interfaces': [], 'dns': []}, errors=['内存：未提供接口', '存储：未提供接口', 'IPv4 默认路由：不可用'])
    page.goto(url + '/system-settings?section=device')
    card = visible_card(page)
    playwright.expect(card.get_by_text('edge-rk3588', exact=True)).to_be_visible()
    playwright.expect(card.get_by_text('未取得本地存储信息。', exact=True)).to_have_count(0)
    playwright.expect(card.get_by_text('未取得网卡信息。', exact=True)).to_have_count(0)
    playwright.expect(card.get_by_text('0.0%', exact=True)).to_have_count(0)
    page.get_by_role('navigation', name='系统设置分类').get_by_role('button', name='性能', exact=False).click()
    card = visible_card(page)
    playwright.expect(card.locator('h4')).to_have_text('性能')
    playwright.expect(card.get_by_text('0.0%', exact=True)).to_have_count(0)
    card.get_by_text('部分信息暂不可用（1 项）', exact=True).click()
    playwright.expect(card.get_by_text('内存：未提供接口', exact=True)).to_be_visible()
    assert not writes


@pytest.mark.parametrize('section,endpoint,label,field,new_value,save_name', [
    ('system', '/api/system/recovery', '卡死复位等待时间（秒）', 'watchdog_timeout_sec', 120, '保存系统恢复设置'),
    ('application', '/api/system/recovery', '单次处理多久未完成视为卡住（秒）', 'task_timeout_sec', 180, '保存设置'),
    ('network', '/api/system/recovery/network', '持续异常确认（秒）', 'failure_timeout_sec', 240, '保存网络恢复策略'),
])
def test_manual_reload_reads_latest_settings_with_feedback_and_never_saves(
        recovery_ui, section, endpoint, label, field, new_value, save_name):
    page, url, snapshots, writes = recovery_ui
    page.goto(url + '/recovery/' + section)
    card = visible_card(page)
    if section != 'system':
        card.locator('.recovery-advanced > summary').click()
    control = card.get_by_label(label, exact=True)
    playwright.expect(control).to_be_visible()
    control.fill(str(new_value + 1))
    status = snapshots[endpoint]
    (status['system']['policy'] if section == 'system' else status['policy'])[field] = new_value
    pending = []
    page.route('**' + endpoint, lambda route: pending.append(route))
    card.get_by_role('button', name='放弃修改并重新读取', exact=True).click()
    reading = card.get_by_role('button', name='读取中……', exact=True)
    playwright.expect(reading).to_be_visible()
    playwright.expect(reading).to_be_disabled()
    playwright.expect(card.get_by_role('button', name=save_name, exact=True)).to_be_disabled()
    assert pending, 'Click must issue a new GET request'
    assert all(route.request.method == 'GET' for route in pending)
    for route in pending:
        route.fulfill(status=200, content_type='application/json', body=json.dumps(status))
    playwright.expect(control).to_have_value(str(new_value))
    playwright.expect(card.get_by_text('已重新读取设备上的设置和最新状态', exact=False)).to_be_visible()
    playwright.expect(card.get_by_text('未保存的修改已放弃', exact=False)).to_be_visible()
    playwright.expect(card.get_by_text('上次手动读取：', exact=False)).to_be_visible()
    assert not writes


@pytest.mark.parametrize('section,endpoint,label', [
    ('system', '/api/system/recovery', '卡死复位等待时间（秒）'),
    ('application', '/api/system/recovery', '单次处理多久未完成视为卡住（秒）'),
    ('network', '/api/system/recovery/network', '持续异常确认（秒）'),
])
def test_failed_manual_reload_preserves_draft_and_reports_error(recovery_ui, section, endpoint, label):
    page, url, _, writes = recovery_ui
    page.goto(url + '/recovery/' + section)
    card = visible_card(page)
    if section != 'system':
        card.locator('.recovery-advanced > summary').click()
    control = card.get_by_label(label, exact=True)
    control.fill('333')
    page.route('**' + endpoint, lambda route: route.fulfill(status=503, content_type='application/json',
               body=json.dumps({'detail': '手动读取暂不可用'})))
    card.get_by_role('button', name='放弃修改并重新读取', exact=True).click()
    playwright.expect(card.get_by_text('手动读取暂不可用', exact=False)).to_be_visible()
    playwright.expect(control).to_have_value('333')
    playwright.expect(card.get_by_text('已重新读取设备上的设置和最新状态', exact=False)).to_have_count(0)
    assert not writes


def test_recovery_history_shows_cause_method_result_evidence_and_category_filter(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    events = [dict(id=category, time=100, updated_at=101, category=category,
                   kind={'system': 'system_boot', 'network': 'reconnect', 'application': 'application_restart'}[category],
                   reason=f'{category} 触发原因', action=f'{category} 恢复方式', result='performed',
                   detail='已执行，原故障是否消除需另行检查。', source='独立恢复服务',
                   evidence='只读系统证据：' + '证据' * 180, time_basis='发现时间', boot_id='boot')
              for category in ('system', 'network', 'application')]
    history = dict(events=events, keep_per_category=20, error=None,
                   observer=dict(updated_at=time.time(), error=None))
    snapshots['/api/system/recovery']['history'] = history
    snapshots['/api/system/recovery/network']['history'] = history
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    card.locator('.recovery-history > summary').click()
    playwright.expect(card.locator('.recovery-history li')).to_have_count(1)
    playwright.expect(card.get_by_text('system 触发原因', exact=False)).to_be_visible()
    playwright.expect(card.get_by_text('system 恢复方式', exact=False)).to_be_visible()
    playwright.expect(card.get_by_text('每类只保留最新 20 条', exact=False)).to_be_visible()
    card.locator('.recovery-history-evidence > summary').first.click()
    for width in (1366, 390):
        page.set_viewport_size({'width': width, 'height': 1100})
        assert card.evaluate('(e) => e.scrollWidth <= e.clientWidth + 1')
    page.get_by_role('button', name='网络故障恢复', exact=False).click()
    card = visible_card(page)
    playwright.expect(card.locator('h4')).to_have_text('网络故障恢复')
    card.locator('.recovery-history > summary').click()
    playwright.expect(card.locator('.recovery-history li')).to_have_count(1)
    playwright.expect(card.get_by_text('network 恢复方式', exact=False)).to_be_visible()
    page.get_by_role('button', name='程序故障恢复', exact=False).click()
    card = visible_card(page)
    playwright.expect(card.locator('h4')).to_have_text('程序故障恢复')
    card.locator('.recovery-history > summary').click()
    playwright.expect(card.locator('.recovery-history li')).to_have_count(1)
    playwright.expect(card.get_by_text('application 触发原因', exact=False)).to_be_visible()
    assert not writes


def test_routine_operations_are_hidden_and_faults_stay_in_their_category(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    def row(key, category, kind, reason, fault=None):
        value = dict(id=key, time=100, updated_at=100, category=category, kind=kind, reason=reason,
                     action='恢复动作', result='performed', detail='结果', source='测试来源', evidence='',
                     time_basis='发现时间', boot_id='boot')
        if fault is not None:
            value['fault_recovery'] = fault
        return value
    events = [row('panic', 'system', 'system_boot', '内核崩溃后重新启动', True),
              row('unknown', 'system', 'system_boot', '异常重启原因未确认', True),
              row('daily', 'system', 'system_boot', '到达每日定时重启时间'),
              row('clean', 'system', 'system_boot', '上一运行周期进入正常关机流程，具体发起原因未确认'),
              row('service', 'system', 'service_replaced', '普通管理服务重新启动'),
              row('manager', 'system', 'manual_service_control', '用户启动管理服务'),
              row('network', 'network', 'reconnect', '网络故障恢复'),
              row('network-boot', 'network', 'system_boot', '网络故障升级整机重启', True),
              row('network-config', 'network', 'manual_network_config', '普通网络设置'),
              row('app', 'application', 'service_failure', '程序崩溃恢复'),
              row('app-start', 'application', 'manual_start', '普通程序启动'),
              row('app-boot', 'application', 'restored', '开机首次自启', False)]
    history = dict(events=events, keep_per_category=20, error=None, observer={'updated_at': time.time()})
    snapshots['/api/system/recovery']['history'] = history
    snapshots['/api/system/recovery/network']['history'] = history
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    card.locator('.recovery-history > summary').click()
    playwright.expect(card.locator('.recovery-history li')).to_have_count(2)
    playwright.expect(card.locator('.recovery-history li').filter(has_text='异常重启原因未确认')).to_be_visible()
    playwright.expect(card.locator('.recovery-history li').filter(has_text='到达每日定时重启时间')).to_have_count(0)
    page.get_by_role('button', name='网络故障恢复', exact=False).click()
    card = visible_card(page)
    playwright.expect(card.locator('h4')).to_have_text('网络故障恢复')
    card.locator('.recovery-history > summary').click()
    playwright.expect(card.locator('.recovery-history li')).to_have_count(2)
    playwright.expect(card.locator('.recovery-history li').filter(has_text='普通网络设置')).to_have_count(0)
    page.get_by_role('button', name='程序故障恢复', exact=False).click()
    card = visible_card(page)
    playwright.expect(card.locator('h4')).to_have_text('程序故障恢复')
    card.locator('.recovery-history > summary').click()
    playwright.expect(card.locator('.recovery-history li')).to_have_count(1)
    playwright.expect(card.locator('.recovery-history li').filter(has_text='普通程序启动')).to_have_count(0)
    assert not writes


def test_recovery_history_read_failure_and_offline_observer_are_visible(recovery_ui):
    page, url, snapshots, _ = recovery_ui
    snapshots['/api/system/recovery']['history'] = dict(events=[], keep_per_category=20,
        error='历史文件不可读', observer=dict(updated_at=1, error='记录目录只读'))
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    card.locator('.recovery-history > summary').click()
    playwright.expect(card.get_by_text('恢复记录读取失败：历史文件不可读', exact=True)).to_be_visible()
    playwright.expect(card.get_by_text('恢复记录服务异常：记录目录只读', exact=True)).to_be_visible()
    playwright.expect(card.get_by_text('尚未确认记录服务正在运行', exact=False)).to_be_visible()


def test_system_page_has_two_independent_switches_without_application_controls(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    original = deepcopy(snapshots['/api/system/recovery']['policy'])
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    playwright.expect(card.get_by_role('checkbox', name='整机卡死自动重启', exact=True)).to_be_checked()
    playwright.expect(card.get_by_role('checkbox', name='系统崩溃自动重启', exact=True)).to_be_checked()
    assert card.locator('input, select').count() == 5
    playwright.expect(card.get_by_role('spinbutton', name='卡死复位等待时间（秒）', exact=True)).to_have_value('89')
    playwright.expect(card.get_by_role('spinbutton', name='重启阶段看门狗超时（秒）', exact=True)).to_have_value('60')
    playwright.expect(card.get_by_role('spinbutton', name='崩溃重启等待时间（秒）', exact=True)).to_have_value('10')
    playwright.expect(card.get_by_role('button', name='保存系统恢复设置')).to_be_disabled()
    assert '应用自动恢复' not in card.inner_text()
    assert '程序退出后自动重启' not in card.inner_text()
    assert not card.locator('.recovery-advanced').evaluate('(e) => e.open')
    card.locator('.recovery-advanced > summary').click()
    playwright.expect(card.get_by_text('dw_wdt', exact=True)).to_be_visible()
    assert snapshots['/api/system/recovery']['policy'] == original and not writes


@pytest.mark.parametrize('state,label,summary', [
    ('device_tree_disabled', '设备树未开启', '检测到看门狗硬件节点被设备树关闭，当前无法通过它保护整机卡死。'),
    ('driver_missing', '缺少内核驱动', '当前内核没有启用所需的硬件看门狗驱动。'),
    ('not_managed', '尚未接管', '已识别硬件看门狗，但系统管理器尚未接管。'),
    ('software_only', '仅发现软件看门狗', '当前只识别到软件看门狗。'),
    ('inspection_failed', '状态未知', '部分系统状态无法读取，目前不能确认整机卡死保护是否生效。'),
])
def test_board_capability_explains_missing_protection_without_disabling_panic_or_writing(recovery_ui, state, label, summary):
    page, url, snapshots, writes = recovery_ui
    snapshots['/api/system/recovery']['hardware'].update(
        enabled=False, device=None, driver=None, state=state, label=label, summary=summary,
        next_steps=['请按板卡厂商说明检查设备树和驱动，再配置系统接管。'],
        diagnostics=dict(devices=[], device_tree=[dict(path='watchdog@feaf0000', status='disabled',
                                                      compatible=['snps,dw-wdt'], disabled_by='watchdog@feaf0000')],
                         kernel_config=dict(DW_WATCHDOG='y', WATCHDOG_SYSFS='n'), registered_drivers=['dw_wdt'], errors=[]))
    snapshots['/api/system/recovery']['system']['policy'].update(hardware_enabled=False, panic_enabled=True)
    snapshots['/api/system/recovery']['system'].update(can_enable_hardware=False, hardware_unavailable_reason=summary)
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    playwright.expect(card.get_by_text(label, exact=True)).to_be_visible()
    playwright.expect(card.get_by_text(summary, exact=True)).to_be_visible()
    playwright.expect(card.get_by_text('请按板卡厂商说明检查设备树和驱动，再配置系统接管。', exact=True)).to_be_visible()
    playwright.expect(card.get_by_text('已开启', exact=True)).to_have_count(1)  # Only the independent panic fallback.
    assert '停止喂狗后约' not in card.inner_text()
    playwright.expect(card.get_by_role('checkbox', name='整机卡死自动重启', exact=True)).to_be_disabled()
    playwright.expect(card.get_by_role('checkbox', name='系统崩溃自动重启', exact=True)).to_be_enabled()
    assert not card.locator('.recovery-advanced').evaluate('(e) => e.open')
    card.locator('.recovery-advanced > summary').click()
    playwright.expect(card.get_by_text('CONFIG_DW_WATCHDOG=y', exact=True)).to_be_visible()
    playwright.expect(card.get_by_text('CONFIG_WATCHDOG_SYSFS=n', exact=True)).to_be_visible()
    for width in (1366, 390):
        page.set_viewport_size({'width': width, 'height': 1100})
        assert card.evaluate('(e) => e.scrollWidth <= e.clientWidth + 1')
    assert not writes


def test_system_panic_controls_follow_live_changes_within_one_second_without_persisting(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    state = snapshots['/api/system/recovery']
    original = deepcopy(state['system']['policy'])
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    panic = card.get_by_role('checkbox', name='系统崩溃自动重启', exact=True)
    delay = card.get_by_role('spinbutton', name='崩溃重启等待时间（秒）', exact=True)
    playwright.expect(panic).to_be_checked()
    for index, timeout in enumerate((0, 29, -1, 0)):
        state['hardware']['panic_timeout_sec'] = timeout
        state['system'].update(revision=f'external-{index}', panic_in_sync=False)
        if timeout == 0:
            playwright.expect(panic).not_to_be_checked(timeout=2500)
            playwright.expect(delay).to_be_disabled()
            playwright.expect(delay).to_have_value('10')
        else:
            playwright.expect(panic).to_be_checked(timeout=2500)
            playwright.expect(delay).to_be_enabled()
            playwright.expect(delay).to_have_value(str(max(0, timeout)), timeout=2500)
        playwright.expect(card.get_by_role('button', name='保存系统恢复设置', exact=True)).to_be_enabled()
    assert state['system']['policy'] == original and not writes


def test_system_can_persist_observed_panic_off_without_cancelling_pending_hardware(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    state = snapshots['/api/system/recovery']
    state['hardware']['panic_timeout_sec'] = 0
    state['system']['policy']['hardware_enabled'] = False
    state['system'].update(hardware_pending=True, panic_in_sync=False)
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    panic = card.get_by_role('checkbox', name='系统崩溃自动重启', exact=True)
    playwright.expect(panic).not_to_be_checked()
    card.get_by_role('button', name='保存系统恢复设置', exact=True).click()
    playwright.expect(card.get_by_role('button', name='保存系统恢复设置', exact=True)).to_be_disabled()
    assert writes[-1][1]['policy'] == dict(hardware_enabled=False, panic_enabled=False,
        watchdog_timeout_sec=89, reboot_timeout_sec=60, panic_timeout_sec=10)
    assert state['system']['hardware_pending']
    playwright.expect(panic).not_to_be_checked()


def test_system_live_panic_change_preserves_unsaved_edits_until_explicit_reload(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    state = snapshots['/api/system/recovery']
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    delay = card.get_by_role('spinbutton', name='崩溃重启等待时间（秒）', exact=True)
    delay.fill('33')
    state['hardware']['panic_timeout_sec'] = 0
    state['system'].update(revision='external-off', panic_in_sync=False)
    playwright.expect(card.get_by_text('系统恢复设置或实际状态已变化', exact=False)).to_be_visible(timeout=2500)
    playwright.expect(delay).to_have_value('33')
    playwright.expect(card.get_by_role('checkbox', name='系统崩溃自动重启', exact=True)).to_be_checked()
    playwright.expect(card.get_by_role('button', name='保存系统恢复设置', exact=True)).to_be_disabled()
    card.get_by_role('button', name='放弃修改并重新读取', exact=True).click()
    playwright.expect(card.get_by_role('checkbox', name='系统崩溃自动重启', exact=True)).not_to_be_checked()
    playwright.expect(delay).to_have_value('10')
    assert not writes


def test_system_switch_saves_are_isolated_and_show_actual_hardware_until_reboot(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    original_app = deepcopy(snapshots['/api/system/recovery']['policy'])
    original_network = deepcopy(snapshots['/api/system/recovery/network']['policy'])
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    hardware = card.get_by_role('checkbox', name='整机卡死自动重启', exact=True)
    panic = card.get_by_role('checkbox', name='系统崩溃自动重启', exact=True)
    hardware.uncheck()
    page.get_by_role('button', name='网络故障恢复', exact=False).click()
    page.get_by_role('button', name='系统故障恢复', exact=False).click()
    playwright.expect(hardware).not_to_be_checked()
    card.get_by_role('button', name='保存系统恢复设置', exact=True).click()
    playwright.expect(card.get_by_text('等待正常重启设备生效', exact=False)).to_be_visible()
    assert snapshots['/api/system/recovery']['hardware']['enabled'] is True
    panic.uncheck()
    card.get_by_role('button', name='保存系统恢复设置', exact=True).click()
    playwright.expect(card.get_by_text('未开启', exact=True)).to_be_visible()
    assert writes[-1] == ('/api/system/recovery/system', dict(
        policy=dict(hardware_enabled=False, panic_enabled=False, watchdog_timeout_sec=89,
                    reboot_timeout_sec=60, panic_timeout_sec=10), revision='switches-1-saved'))
    assert snapshots['/api/system/recovery']['policy'] == original_app
    assert snapshots['/api/system/recovery/network']['policy'] == original_network
    assert all(path == '/api/system/recovery/system' for path, _ in writes)


def test_system_switch_conflict_keeps_draft_and_explicit_reload_restores_server_choice(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    hardware = card.get_by_role('checkbox', name='整机卡死自动重启', exact=True)
    hardware.uncheck()
    snapshots['/api/system/recovery']['system']['revision'] = 'other-page'
    playwright.expect(card.get_by_text('系统恢复设置或实际状态已变化', exact=False)).to_be_visible(timeout=10000)
    playwright.expect(hardware).not_to_be_checked()
    playwright.expect(card.get_by_role('button', name='保存系统恢复设置', exact=True)).to_be_disabled()
    card.get_by_role('button', name='放弃修改并重新读取', exact=True).click()
    playwright.expect(hardware).to_be_checked()
    assert not writes


def test_system_timeout_edits_save_all_values_and_keep_actual_state_until_reboot(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    card.get_by_role('spinbutton', name='卡死复位等待时间（秒）', exact=True).fill('120')
    card.get_by_role('spinbutton', name='重启阶段看门狗超时（秒）', exact=True).fill('180')
    card.get_by_role('spinbutton', name='崩溃重启等待时间（秒）', exact=True).fill('25')
    page.get_by_role('button', name='网络故障恢复', exact=False).click()
    page.get_by_role('button', name='系统故障恢复', exact=False).click()
    playwright.expect(card.get_by_role('spinbutton', name='卡死复位等待时间（秒）', exact=True)).to_have_value('120')
    card.get_by_role('button', name='保存系统恢复设置', exact=True).click()
    playwright.expect(card.get_by_text('等待正常重启设备生效', exact=False)).to_be_visible()
    assert writes == [('/api/system/recovery/system', dict(policy=dict(hardware_enabled=True, panic_enabled=True,
        watchdog_timeout_sec=120, reboot_timeout_sec=180, panic_timeout_sec=25), revision='switches-1'))]
    playwright.expect(card.get_by_text('停止喂狗后约 1 分钟 29 秒 触发硬件复位', exact=True)).to_be_visible()
    playwright.expect(card.get_by_text('内核报告严重错误后约 25 秒后尝试重启', exact=True)).to_be_visible()
    assert snapshots['/api/system/recovery']['hardware']['runtime_timeout'] == '1min 29s'
    card.get_by_role('spinbutton', name='崩溃重启等待时间（秒）', exact=True).fill('30')
    card.get_by_role('button', name='保存系统恢复设置', exact=True).click()
    playwright.expect(card.get_by_text('等待正常重启设备生效', exact=False)).to_be_visible()
    for width in (1366, 390):
        page.set_viewport_size({'width': width, 'height': 1100})
        assert card.evaluate('(e) => e.scrollWidth <= e.clientWidth + 1')


def test_panic_zero_means_immediate_and_switch_off_retains_the_delay(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    delay = card.get_by_role('spinbutton', name='崩溃重启等待时间（秒）', exact=True)
    delay.fill('0')
    card.get_by_role('button', name='保存系统恢复设置', exact=True).click()
    playwright.expect(card.get_by_text('内核报告严重错误后立即尝试重启', exact=True)).to_be_visible()
    assert writes[-1][1]['policy']['panic_timeout_sec'] == -1
    assert not snapshots['/api/system/recovery']['system']['hardware_pending']
    panic = card.get_by_role('checkbox', name='系统崩溃自动重启', exact=True)
    panic.uncheck()
    playwright.expect(delay).to_be_disabled()
    card.get_by_role('button', name='保存系统恢复设置', exact=True).click()
    playwright.expect(card.get_by_text('未开启', exact=True)).to_be_visible()
    panic.check()
    playwright.expect(delay).to_have_value('0')
    card.get_by_role('button', name='保存系统恢复设置', exact=True).click()
    playwright.expect(card.get_by_text('内核报告严重错误后立即尝试重启', exact=True)).to_be_visible()


@pytest.mark.parametrize('label,value,message', [
    ('卡死复位等待时间（秒）', '', '卡死复位等待时间必须为 1～86400 秒的整数'),
    ('卡死复位等待时间（秒）', '0', '卡死复位等待时间必须为 1～86400 秒的整数'),
    ('重启阶段看门狗超时（秒）', '86401', '重启阶段看门狗超时必须为 1～86400 秒的整数'),
    ('崩溃重启等待时间（秒）', '1.5', '崩溃重启等待时间必须为 0～2147483647 秒的整数，0 表示立即重启'),
])
def test_invalid_system_timeouts_are_explained_without_sending_a_save(recovery_ui, label, value, message):
    page, url, _, writes = recovery_ui
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    card.get_by_role('spinbutton', name=label, exact=True).fill(value)
    card.get_by_role('button', name='保存系统恢复设置', exact=True).click()
    playwright.expect(card.get_by_text(message, exact=True)).to_be_visible()
    assert not writes


def test_hardware_disabled_by_user_is_shown_as_closed_after_application(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    status = snapshots['/api/system/recovery']
    status['hardware'].update(enabled=False, label='尚未接管', state='not_managed')
    status['system'].update(configured=True)
    status['system']['policy'].update(hardware_enabled=False, panic_enabled=True)
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    playwright.expect(card.get_by_text('已关闭', exact=True)).to_be_visible()
    playwright.expect(card.get_by_role('checkbox', name='整机卡死自动重启', exact=True)).not_to_be_checked()
    assert not writes


def test_system_switch_save_failure_keeps_user_draft_and_actual_state(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    panic = card.get_by_role('checkbox', name='系统崩溃自动重启', exact=True)
    panic.uncheck()
    page.route('**/api/system/recovery/system', lambda route: route.fulfill(
        status=503, content_type='application/json', body=json.dumps({'detail': '保存失败，已恢复原设置'})))
    card.get_by_role('button', name='保存系统恢复设置', exact=True).click()
    playwright.expect(card.get_by_text('保存失败，已恢复原设置', exact=True)).to_be_visible()
    playwright.expect(panic).not_to_be_checked()
    assert snapshots['/api/system/recovery']['hardware']['panic_timeout_sec'] == 10
    assert not writes


def test_status_refresh_failure_marks_previously_active_protection_as_stale(recovery_ui):
    page, url, _, writes = recovery_ui
    page.goto(url + '/recovery/system')
    card = visible_card(page)
    playwright.expect(card.get_by_text('已开启', exact=True)).to_have_count(2)
    page.route('**/api/system/recovery', lambda route: route.fulfill(
        status=503, content_type='application/json', body=json.dumps({'detail': '巡检暂不可用'})))
    playwright.expect(card.get_by_text('状态未更新', exact=True)).to_have_count(2, timeout=10000)
    playwright.expect(card.get_by_text('已开启', exact=True)).to_have_count(0)
    assert not writes


@pytest.mark.parametrize('enabled', [False, True])
def test_missing_network_dependencies_explain_unavailability_and_still_allow_disabling(recovery_ui, enabled):
    page, url, snapshots, writes = recovery_ui
    network = snapshots['/api/system/recovery/network']
    network['requirements'] = dict(ready=False, missing_tools=['nmcli'], message='缺少网络恢复所需工具：nmcli。请管理员安装 NetworkManager 后再开启。')
    network['policy'].update(enabled=enabled, targets=[dict(host='192.0.2.10', method='ping')])
    page.goto(url + '/recovery/network')
    card = visible_card(page)
    playwright.expect(card.get_by_text('暂不可用', exact=True)).to_be_visible()
    playwright.expect(card.get_by_text(network['requirements']['message'], exact=True)).to_be_visible()
    switch = card.get_by_role('checkbox', name='网络失联自动恢复', exact=False)
    if enabled:
        playwright.expect(switch).to_be_enabled()
        switch.uncheck()
        card.get_by_role('button', name='保存网络恢复策略', exact=True).click()
        playwright.expect(card.get_by_text('网络恢复策略已保存', exact=False)).to_be_visible()
        assert writes[-1][1]['policy']['enabled'] is False
    else:
        playwright.expect(switch).to_be_disabled()
        assert not writes


def test_network_basic_setup_preserves_draft_and_requires_target(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    original = deepcopy(snapshots['/api/system/recovery/network']['policy'])
    page.goto(url + '/recovery/network')
    card = visible_card(page)
    save = card.get_by_role('button', name='保存网络恢复策略', exact=True)
    playwright.expect(save).to_be_disabled()
    assert card.locator('input:visible').count() == 3
    playwright.expect(card.get_by_label('检测方式', exact=True)).to_have_value('ping')
    playwright.expect(card.get_by_label('服务端口', exact=True)).to_have_count(0)
    card.get_by_role('checkbox', name='网络失联自动恢复', exact=False).check()
    save.click()
    playwright.expect(card.get_by_text('请填写网络检测目标地址', exact=False)).to_be_visible()
    assert not writes
    card.get_by_label('网络检测目标地址', exact=True).fill('  192.0.2.20  ')
    card.get_by_role('checkbox', name='必要时允许重启盒子', exact=False).check()
    page.get_by_role('button', name='系统故障恢复', exact=False).click()
    page.get_by_role('button', name='网络故障恢复', exact=False).click()
    playwright.expect(card.get_by_label('网络检测目标地址', exact=True)).to_have_value('  192.0.2.20  ')
    save.click()
    playwright.expect(card.get_by_text('网络恢复策略已保存', exact=False)).to_be_visible()
    assert writes[-1][1]['policy'] == {**original, 'enabled': True, 'allow_reboot': True,
                                     'targets': [{'host': '192.0.2.20', 'method': 'ping'}]}
    for width in (1366, 390):
        page.set_viewport_size({'width': width, 'height': 1100})
        assert card.evaluate('(e) => e.scrollWidth <= e.clientWidth + 1')
        playwright.expect(card.get_by_label('网络检测目标地址', exact=True)).to_be_visible()
        assert card.get_by_role('checkbox', name='网络失联自动恢复', exact=False).bounding_box()['width'] >= 16


def test_network_hidden_custom_values_and_secondary_targets_survive_basic_save(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    policy = snapshots['/api/system/recovery/network']['policy']
    policy.update(enabled=True, interface='eth0', failure_timeout_sec=240, recovery_wait_sec=90,
                  action_cooldown_sec=420, allow_manager_restart=False, allow_reboot=True,
                  reboot_after_sec=1200, reboot_cooldown_sec=7200, max_reboots_per_day=3,
                  targets=[{'host': 'primary.example.test', 'port': 443},
                           {'host': 'backup.example.test', 'port': 8443}])
    original = deepcopy(policy)
    page.goto(url + '/recovery/network')
    card = visible_card(page)
    host = card.get_by_label('网络检测目标地址', exact=True)
    playwright.expect(host).to_have_value('primary.example.test')
    playwright.expect(card.get_by_label('检测方式', exact=True)).to_have_value('tcp')
    playwright.expect(card.get_by_label('服务端口', exact=True)).to_have_value('443')
    host.fill('changed.example.test')
    card.get_by_role('button', name='保存网络恢复策略', exact=True).click()
    playwright.expect(card.get_by_text('网络恢复策略已保存', exact=False)).to_be_visible()
    expected = deepcopy(original)
    expected['targets'][0]['host'] = 'changed.example.test'
    assert writes[-1][1]['policy'] == expected
    assert not card.locator('.recovery-advanced').evaluate('(e) => e.open')

    card.locator('.recovery-advanced > summary').click()
    card.get_by_role('button', name='添加备用目标', exact=True).click()
    card.locator('.recovery-advanced > summary').click()
    card.get_by_role('button', name='保存网络恢复策略', exact=True).click()
    playwright.expect(card.get_by_label('备用目标地址 2', exact=True)).to_be_visible()
    assert len(writes) == 1


def test_switching_detection_method_removes_ping_port_and_retains_backup(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    policy = snapshots['/api/system/recovery/network']['policy']
    backup = {'host': '192.0.2.30', 'method': 'ping'}
    policy.update(enabled=True, targets=[{'host': '192.0.2.20', 'port': 8443}, backup])
    page.goto(url + '/recovery/network')
    card = visible_card(page)
    method = card.get_by_label('检测方式', exact=True)
    playwright.expect(method).to_have_value('tcp')
    method.select_option('ping')
    playwright.expect(card.get_by_label('服务端口', exact=True)).to_have_count(0)
    save = card.get_by_role('button', name='保存网络恢复策略', exact=True)
    save.click()
    playwright.expect(card.get_by_text('网络恢复策略已保存', exact=False)).to_be_visible()
    playwright.expect(save).to_be_disabled()
    assert writes[-1][1]['policy']['targets'] == [{'host': '192.0.2.20', 'method': 'ping'}, backup]
    method.select_option('tcp')
    card.get_by_label('服务端口', exact=True).fill('9443')
    save.click()
    playwright.expect(card.get_by_text('网络恢复策略已保存', exact=False)).to_be_visible()
    playwright.expect(save).to_be_disabled()
    assert writes[-1][1]['policy']['targets'] == [{'host': '192.0.2.20', 'port': 9443}, backup]


def application_setup(snapshots, running=False):
    status = snapshots['/api/system/recovery']
    status['policy'].update(app_restart_enabled=False, app_stall_enabled=False)
    if running:
        status['current'] = dict(app='demo', pid=123, config='production.json', restart_enabled=False, watchdog_active=False)
    return status


def test_application_basic_switches_link_and_preserve_advanced_settings_across_categories(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    status = application_setup(snapshots)
    original = deepcopy(status['policy'])
    page.goto(url + '/recovery/application')
    card = visible_card(page)
    restart = card.get_by_role('checkbox', name='程序退出后自动重启', exact=False)
    stall = card.get_by_role('checkbox', name='程序卡住后自动恢复', exact=False)
    save = card.get_by_role('button', name='保存设置', exact=True)
    playwright.expect(save).to_be_disabled()
    assert card.locator('input:visible').count() == 2
    stall.check()
    playwright.expect(restart).to_be_checked()
    restart.uncheck()
    playwright.expect(stall).not_to_be_checked()
    stall.check()
    page.get_by_role('button', name='系统故障恢复', exact=False).click()
    page.get_by_role('button', name='网络故障恢复', exact=False).click()
    page.get_by_role('button', name='程序故障恢复', exact=False).click()
    playwright.expect(stall).to_be_checked()
    save.click()
    playwright.expect(card.get_by_text('设置已保存，程序下次启动时使用。', exact=True)).to_be_visible()
    assert writes == [('/api/system/recovery/application', {'revision': 'system-1', 'policy': {
        'app_restart_enabled': True, 'app_stall_enabled': True,
        'restart_delay_sec': original['restart_delay_sec'], 'task_timeout_sec': original['task_timeout_sec'],
        'startup_timeout_sec': original['startup_timeout_sec'],
    }})]
    assert status['policy'] == {**original, 'app_restart_enabled': True, 'app_stall_enabled': True}
    assert card.get_by_role('button', name='重启程序并生效', exact=True).count() == 0
    for width in (1366, 390):
        page.set_viewport_size({'width': width, 'height': 1100})
        assert card.evaluate('(e) => e.scrollWidth <= e.clientWidth + 1')
        assert restart.bounding_box()['width'] >= 16


def test_application_save_does_not_restart_and_explicit_apply_updates_actual_protection(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    status = application_setup(snapshots, running=True)
    page.goto(url + '/recovery/application')
    card = visible_card(page)
    card.get_by_role('checkbox', name='程序卡住后自动恢复', exact=False).check()
    card.get_by_role('button', name='保存设置', exact=True).click()
    apply = card.get_by_role('button', name='重启程序并生效', exact=True)
    playwright.expect(apply).to_be_visible()
    assert len(writes) == 1 and writes[0][0] == '/api/system/recovery/application'
    playwright.expect(card.get_by_text('退出重启：未开启', exact=True)).to_be_visible()
    playwright.expect(card.get_by_text('重启程序会短暂中断视频和推理。', exact=False)).to_be_visible()
    apply.click()
    playwright.expect(card.get_by_text('退出重启：已生效', exact=True)).to_be_visible()
    playwright.expect(card.get_by_text('卡住恢复：已生效', exact=False)).to_be_visible()
    assert writes[-1] == ('/api/system/recovery/application/apply', {'revision': status['revision'], 'app': 'demo'})


def test_application_reports_unavailable_stall_detection_without_claiming_it_is_active(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    status = application_setup(snapshots, running=True)
    status['policy'].update(app_restart_enabled=True, app_stall_enabled=True)
    status['current']['restart_enabled'] = True
    page.goto(url + '/recovery/application')
    card = visible_card(page)
    playwright.expect(card.get_by_text('程序包可能不支持此功能', exact=False)).to_be_visible()
    playwright.expect(card.get_by_text('卡住恢复：未生效', exact=True)).to_be_visible()
    assert not writes


def test_application_invalid_hidden_timeout_is_revealed_and_recommended_times_keep_switches(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    application_setup(snapshots)
    page.goto(url + '/recovery/application')
    card = visible_card(page)
    stall = card.get_by_role('checkbox', name='程序卡住后自动恢复', exact=False)
    stall.check()
    card.locator('.recovery-advanced > summary').click()
    timeout = card.get_by_label('单次处理多久未完成视为卡住（秒）', exact=True)
    timeout.fill('1')
    card.locator('.recovery-advanced > summary').click()
    card.get_by_role('button', name='保存设置', exact=True).click()
    playwright.expect(timeout).to_be_visible()
    assert not writes
    card.get_by_role('button', name='恢复推荐时间', exact=True).click()
    playwright.expect(timeout).to_have_value('120')
    playwright.expect(stall).to_be_checked()
    card.get_by_role('button', name='保存设置', exact=True).click()
    playwright.expect(card.get_by_text('设置已保存，程序下次启动时使用。', exact=True)).to_be_visible()
    assert writes[-1][1]['policy']['restart_delay_sec'] == 5
    assert writes[-1][1]['policy']['startup_timeout_sec'] == 300


def test_application_external_policy_change_keeps_draft_and_requires_reread(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    status = application_setup(snapshots, running=True)
    page.goto(url + '/recovery/application')
    card = visible_card(page)
    restart = card.get_by_role('checkbox', name='程序退出后自动重启', exact=False)
    restart.check()
    status['revision'] = 'external-change'
    status['policy']['task_timeout_sec'] = 200
    playwright.expect(card.get_by_text('设置已在其他页面更新', exact=False)).to_be_visible(timeout=8000)
    playwright.expect(restart).to_be_checked()
    playwright.expect(card.get_by_role('button', name='保存设置', exact=True)).to_be_disabled()
    card.locator('.device-settings-note.warning').get_by_role('button', name='放弃修改并重新读取', exact=True).click()
    playwright.expect(restart).not_to_be_checked()
    assert not writes


def performance_snapshot():
    devices = []
    for kind, max_freq in [('CPU', 1704), ('GPU', 850), ('NPU', 950)]:
        devices.append(dict(id=kind + ':0', kind=kind, label=kind + (' 核心 0 1 2 3' if kind == 'CPU' else ''),
                            cores='0 1 2 3' if kind == 'CPU' else '', current_mhz=600, min_mhz=300, max_mhz=max_freq,
                            available_mhz=[300, 600, max_freq], governor='ondemand', usage_percent=None if kind == 'CPU' else 0,
                            usage_source=None if kind == 'CPU' else '/sys/kernel/debug/rknpu/load' if kind == 'NPU' else 'devfreq/load',
                            core_loads=[dict(core=i, usage_percent=0) for i in range(3)] if kind == 'NPU' else [],
                            temperature_celsius=48.5, driver='RKNPU v0.9.0' if kind == 'NPU' else None, editable=True))
    return dict(sampled_at=time.time(), devices=devices,
                thermal_trips=[dict(id='thermal:soc:0', zone='soc-thermal', kind='passive', celsius=75,
                                    hysteresis_celsius=2, default_celsius=75, ceiling_celsius=None, editable=True, sensor_celsius=48.5),
                               dict(id='thermal:soc:1', zone='soc-thermal', kind='critical', celsius=115,
                                    hysteresis_celsius=2, default_celsius=115, ceiling_celsius=115, editable=True, sensor_celsius=48.5)],
                revision='performance-1', saved=False, applied=False, profile_matches=False, error=None, notice=None)


def test_performance_polling_is_one_second_stops_on_leave_and_has_bounded_graph(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    requests = []
    page.on('request', lambda r: requests.append(time.monotonic()) if urlsplit(r.url).path == '/api/system/device-info' else None)
    page.goto(url + '/system-settings?section=performance')
    card = visible_card(page)
    playwright.expect(card.locator('h4')).to_have_text('性能')
    page.wait_for_timeout(2300)
    assert 3 <= len(requests) <= 4
    assert .7 < requests[2] - requests[1] < 1.5
    card.get_by_role('tab', name='NPU').click()
    for core in range(3):
        playwright.expect(card.get_by_text(f'核心 {core}：0.0%', exact=True)).to_be_visible()
    playwright.expect(card.locator('.performance-graph path[data-npu-core]')).to_have_count(3)
    assert int(card.locator('.performance-graph').get_attribute('data-samples')) >= 3
    # Speed up clock, but allow requests to complete between ticks.
    page.clock.install()
    for _ in range(65):
        with page.expect_response('**/api/system/device-info'):
            page.clock.fast_forward(1000)
    assert card.locator('.performance-graph').get_attribute('data-samples') == '60'
    for core in range(3):
        path = card.locator(f'.performance-graph path[data-npu-core="{core}"]').get_attribute('d')
        assert len(re.findall('[ML]', path)) <= 60
    page.get_by_role('link', name='故障恢复', exact=False).click()
    count = len(requests)
    page.clock.run_for(3000)
    assert len(requests) == count and not writes


def test_performance_polling_preserves_draft_and_saves_explicitly(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    page.goto(url + '/system-settings?section=performance')
    card = visible_card(page)
    card.get_by_text('性能与温控设置', exact=True).click()
    control = card.get_by_role('combobox', name='CPU 核心 0 1 2 3最大频率')
    control.select_option('600')
    threshold = card.get_by_role('spinbutton', name='soc-thermal 降频阈值', exact=False)
    threshold.fill('70')
    page.wait_for_timeout(1250)
    playwright.expect(control).to_have_value('600')
    playwright.expect(threshold).to_have_value('70')
    assert not writes
    def save(route):
        payload = route.request.post_data_json
        assert payload['revision'] == 'performance-1'
        assert payload['limits']['CPU:0'] == 600 and payload['limits']['thermal:soc:0'] == 70
        writes.append(('performance', payload))
        result = snapshots['/api/system/device-info']['performance']
        result['devices'][0]['max_mhz'] = 600
        result['thermal_trips'][0]['celsius'] = 70
        result.update(revision='performance-2', saved=True, applied=True, profile_matches=True)
        route.fulfill(status=200, content_type='application/json', body=json.dumps(result))
    page.route('**/api/system/device-info/performance', save)
    card.get_by_role('button', name='保存并应用', exact=True).click()
    playwright.expect(card.get_by_role('status')).to_have_text('已保存并生效。')
    playwright.expect(card.get_by_role('button', name='保存并应用', exact=True)).to_be_disabled()
    assert len(writes) == 1


def test_performance_failure_preserves_draft_and_explains_conflict(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    page.route('**/api/system/device-info/performance', lambda route: route.fulfill(
        status=409, content_type='application/json', body=json.dumps({'detail': '性能参数已变化，请放弃修改后按当前值重新设置'})))
    page.goto(url + '/system-settings?section=performance')
    card = visible_card(page)
    card.get_by_text('性能与温控设置', exact=True).click()
    control = card.get_by_role('combobox', name='CPU 核心 0 1 2 3最大频率')
    control.select_option('600')
    card.get_by_role('button', name='保存并应用', exact=True).click()
    playwright.expect(card.get_by_role('status')).to_contain_text('性能参数已变化')
    playwright.expect(control).to_have_value('600')
    card.get_by_role('button', name='放弃修改', exact=True).click()
    playwright.expect(control).to_have_value('1704')


def test_performance_polling_pauses_when_hidden_and_avoids_overlapping_requests(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    requests = []
    pending = []
    def delayed(route):
        requests.append(time.monotonic())
        pending.append(route)
    page.route('**/api/system/device-info', delayed)
    page.goto(url + '/system-settings?section=performance')
    page.wait_for_timeout(2200)
    assert len(requests) == 1
    page.evaluate("Object.defineProperty(document, 'visibilityState', {configurable:true, get:()=> 'hidden'}); document.dispatchEvent(new Event('visibilitychange'))")
    pending.pop().fulfill(status=200, content_type='application/json', body=json.dumps(snapshots['/api/system/device-info']))
    playwright.expect(visible_card(page).locator('.performance-monitor')).to_be_visible()
    count = len(requests)
    page.wait_for_timeout(1300)
    assert len(requests) == count
    page.evaluate("Object.defineProperty(document, 'visibilityState', {configurable:true, get:()=> 'visible'}); document.dispatchEvent(new Event('visibilitychange'))")
    page.wait_for_timeout(100)
    assert len(requests) == count + 1
    pending.pop().fulfill(status=200, content_type='application/json', body=json.dumps(snapshots['/api/system/device-info']))
    assert not writes


def test_npu_core_curves_show_independent_colored_history_and_missing_readings(recovery_ui):
    page, url, snapshots, writes = recovery_ui
    npu = next(d for d in snapshots['/api/system/device-info']['performance']['devices'] if d['kind'] == 'NPU')
    npu['core_loads'] = [dict(core=i, usage_percent=v) for i, v in enumerate([20, 50, 80])]
    npu['usage_percent'] = 50
    page.goto(url + '/system-settings?section=performance')
    card = visible_card(page)
    card.get_by_role('tab', name='NPU').click()
    legend = card.get_by_role('list', name='NPU 核心图例')
    colors = []
    for core, value in enumerate([20, 50, 80]):
        playwright.expect(legend.get_by_text(f'核心 {core}：{value:.1f}%', exact=True)).to_be_visible()
        curve = card.locator(f'.performance-graph path[data-npu-core="{core}"]')
        playwright.expect(curve).to_have_attribute('d', re.compile(rf'.*,{180 * (1 - value / 100):.1f}$'))
        color = curve.evaluate('e => getComputedStyle(e).stroke')
        assert legend.locator('li').nth(core).evaluate('e => getComputedStyle(e).color') == color
        colors.append(color)
    assert len(set(colors)) == 3
    assert card.get_by_role('tab', name='NPU').locator('svg path').count() == 3
    before = [card.locator(f'.performance-graph path[data-npu-core="{core}"]').get_attribute('d') for core in range(3)]
    npu['core_loads'] = [dict(core=0, usage_percent=10), dict(core=2, usage_percent=90)]
    playwright.expect(legend.get_by_text('核心 0：10.0%', exact=True)).to_be_visible()
    playwright.expect(legend.get_by_text('核心 1：未提供', exact=True)).to_be_visible()
    playwright.expect(legend.get_by_text('核心 2：90.0%', exact=True)).to_be_visible()
    curve0 = card.locator('.performance-graph path[data-npu-core="0"]')
    curve1 = card.locator('.performance-graph path[data-npu-core="1"]')
    curve2 = card.locator('.performance-graph path[data-npu-core="2"]')
    assert 'L' in curve0.get_attribute('d') and curve0.get_attribute('d').endswith(',162.0')
    assert curve2.get_attribute('d').endswith(',18.0')
    assert curve1.get_attribute('d').strip().endswith(',90.0')  # Missing values leave a gap, not a false zero.
    assert curve0.get_attribute('d') != before[0]
    npu['core_loads'] = [dict(core=i, usage_percent=0) for i in range(3)]
    for core in range(3):
        playwright.expect(legend.get_by_text(f'核心 {core}：0.0%', exact=True)).to_be_visible()
    playwright.expect(curve1).to_have_attribute('d', re.compile(r'M[^M]+M.*180.0$'))
    # Solid / dashed / dotted styles also distinguish coincident zero-load curves.
    assert [card.locator(f'.performance-graph path[data-npu-core="{i}"]').get_attribute('stroke-dasharray') for i in range(3)] == [None, '8 4', '2 4']
    for width in (1366, 390):
        page.set_viewport_size({'width': width, 'height': 1100})
        assert card.evaluate('e => e.scrollWidth <= e.clientWidth + 1')
    assert not writes
