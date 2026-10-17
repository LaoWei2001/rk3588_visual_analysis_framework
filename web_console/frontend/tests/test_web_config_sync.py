"""External configuration changes, dirty forms and recovery without device writes."""
from copy import deepcopy
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import threading
import time
from urllib.parse import urlsplit

import pytest

pw_api = pytest.importorskip('playwright.sync_api')
DIST = Path(__file__).resolve().parents[1] / 'dist'


@pytest.fixture
def console():
    app = dict(name='demo', path='/isolated/demo', has_binary=True, has_config=True,
               models=[], labels=[], videos=[], config_files=['config.json'], active_config='config.json',
               unreported=0, autostart=False, desired_running=False, status='stopped', pid=None, uptime_seconds=None)
    interface = dict(device='eth0', type='ethernet', state='connected', connection='wired', connection_uuid='wired-1',
                     mac='00:11:22:33:44:55', addresses=['192.0.2.10/24'], gateway='192.0.2.1', dns=['192.0.2.53'],
                     ipv4_method='manual', ssid='', configurable=True)
    camera = dict(version=1, interface='eth0', camera_ip='192.0.2.50', camera_prefix_length=24,
                  camera_network='192.0.2.0/24', camera_mac='', model='', serial='', http_port=80, rtsp_port=554,
                  http_port_inferred=False, rtsp_port_inferred=False, local_ip='192.0.2.51', local_prefix_length=32,
                  isolation_mode='host-route', conflicts=[], warnings=[], address_preexisting=False,
                  route_preexisting=False, address_owned=True, route_owned=True, applied_at=1)
    snapshots = {
        '/api/apps': [app],
        '/api/system/daily-reboot': dict(enabled=False, configured_enabled=False, time='04:00', installed=True,
            active=False, unit_enabled=False, next_run=None, timezone='Asia/Shanghai', current_time='',
            current_time_epoch_ms=int(time.time() * 1000), utc_offset_minutes=480, error=None,
            ntp_enabled=False, ntp_synchronized=False, can_ntp=True, time_error=None),
        '/api/system/timezones': dict(timezones=['Asia/Shanghai', 'UTC']),
        '/api/system/storage': dict(auto_cleanup=False, retention_days=30, max_event_store_gb=1, min_free_gb=1,
            storage_path='/isolated', total_bytes=10000, used_bytes=4000, free_bytes=6000, used_percent=40,
            event_bytes=0, event_count=0, root_cleanup_targets=[], last_cleanup=None),
        '/api/system/network': dict(hostname='before-host', interfaces=[interface], connections=[],
            pending_transaction=None, config_supported=True, error=None),
        '/api/system/camera': dict(interfaces=[dict(device='eth0', mac=interface['mac'], link_up=True,
            addresses=['192.0.2.51/32'], has_default_route=False, configured=True)], configuration=camera,
            status=None, status_error=None, error=None),
        '/api/services': [dict(key='gpio_state', label='GPIO 服务', unit='isolated.service', scope='system',
            installed=True, active_state='active', sub_state='running', enabled=False, autostart=False,
            desired_running=True, uptime_seconds=1, n_restarts=0, bound_app=None, working_dir='/isolated', path_ok=True)],
        '/api/console/info': dict(version='test', apps_root='/isolated', binary_name='vision_analysis', known_model_types=[]),
        '/api/apps/demo/config': dict(global_ignored=True, channels=[], **{'global': {'max_fps': 25}}),
        '/api/apps/demo/config-files': ['assets/config.json'],
        '/api/apps/demo/assets': dict(models=[], labels=[], videos=[]),
        '/api/apps/demo/connections': dict(connections={}),
        '/api/apps/demo/logics': dict(channel_logics=[], global_logics=[], model_types=[]),
        '/api/apps/demo/delivery-adapters': dict(adapters=[]),
        '/api/apps/demo/report-contracts': dict(contracts=[]),
        '/api/apps/demo/ota-config': dict(platform_ws_host='before.example'),
    }
    writes, requests, errors = [], [], []

    class Handler(SimpleHTTPRequestHandler):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, directory=str(DIST), **kwargs)

        def do_GET(self):
            if not self.path.startswith('/assets/'):
                self.path = '/index.html'
            super().do_GET()

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        with pw_api.sync_playwright() as pw:
            browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
            context = browser.new_context(viewport={'width': 1500, 'height': 1100})
            context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'isolated',username:'tester'},version:0}))")
            page = context.new_page()
            page.on('pageerror', lambda error: errors.append(str(error)))

            def api(route):
                request = route.request
                path = urlsplit(request.url).path
                requests.append((request.method, path))
                if request.method != 'GET':
                    writes.append((path, request.post_data_json))
                    route.fulfill(status=500, content_type='application/json', body='{"detail":"unexpected write"}')
                    return
                assert path in snapshots, path
                route.fulfill(content_type='application/json', body=json.dumps(deepcopy(snapshots[path])))

            page.route('**/api/**', api)
            yield page, f'http://127.0.0.1:{server.server_port}', snapshots, writes, requests
            assert not errors, errors
            assert not writes, writes
            browser.close()
    finally:
        server.shutdown(); server.server_close(); worker.join(timeout=5)


def test_system_timezone_and_reboot_follow_external_changes_and_protect_edits(console):
    page, url, snapshots, _, _ = console
    state = snapshots['/api/system/daily-reboot']
    page.goto(url + '/system-settings?section=system')
    timezone = page.get_by_role('combobox', name='修改时区', exact=True)
    pw_api.expect(timezone).to_have_value('Asia/Shanghai')
    state.update(timezone='UTC', utc_offset_minutes=0, enabled=True, active=True, unit_enabled=True, time='12:34')
    pw_api.expect(timezone).to_have_value('UTC', timeout=2500)
    pw_api.expect(page.get_by_role('checkbox')).to_be_checked()
    selects = page.locator('.system-time-picker select')
    pw_api.expect(selects.nth(0)).to_have_value('12')
    pw_api.expect(selects.nth(1)).to_have_value('34')
    selects.nth(1).select_option('35')
    state['time'] = '13:36'
    pw_api.expect(page.locator('.config-sync-notice')).to_contain_text('底层配置已变化', timeout=2500)
    pw_api.expect(selects.nth(1)).to_have_value('35')
    pw_api.expect(page.get_by_role('button', name='保存重启设置')).to_be_disabled()
    page.get_by_role('button', name='放弃修改并同步').click()
    pw_api.expect(selects.nth(0)).to_have_value('13')
    pw_api.expect(selects.nth(1)).to_have_value('36')


@pytest.mark.parametrize('ntp', [False, True])
def test_manual_clock_uses_device_timezone_preserves_edit_and_shows_applied_result(console, ntp):
    from datetime import datetime
    from zoneinfo import ZoneInfo
    page, url, snapshots, _, _ = console
    state = snapshots['/api/system/daily-reboot']
    state['ntp_enabled'] = ntp
    saved = []
    target = '2026-10-20T12:34:56'

    def save(route):
        saved.append(route.request.post_data_json)
        state.update(ntp_enabled=False, current_time_epoch_ms=int(
            datetime(2026, 10, 20, 12, 34, 56, tzinfo=ZoneInfo('Asia/Shanghai')).timestamp() * 1000))
        route.fulfill(content_type='application/json', body=json.dumps(state))

    page.route('**/api/system/time', save)
    page.goto(url + '/system-settings?section=system')
    control = page.get_by_label('修改日期与时间', exact=True)
    control.fill(target)
    page.wait_for_timeout(1200)
    pw_api.expect(control).to_have_value(target)
    page.get_by_role('button', name='关闭自动校时并应用时间' if ntp else '应用时间', exact=True).click()
    pw_api.expect(page.get_by_text('系统时间已修改', exact=True)).to_be_visible()
    assert saved == [{'timezone': 'Asia/Shanghai', 'disable_ntp': ntp, 'local_time': target}]
    pw_api.expect(page.locator('.system-clock-panel strong')).to_contain_text('2026-10-20 12:34:')
    pw_api.expect(page.get_by_role('button', name='应用时间', exact=True)).to_be_disabled()
    for width in (1500, 390):
        page.set_viewport_size({'width': width, 'height': 1100})
        assert page.locator('.settings-section').evaluate('e => e.scrollWidth <= e.clientWidth + 1')


def test_computer_sync_submits_absolute_browser_timestamp(console):
    page, url, snapshots, _, _ = console
    saved = []
    page.route('**/api/system/time', lambda route: (
        saved.append(route.request.post_data_json),
        route.fulfill(content_type='application/json', body=json.dumps(snapshots['/api/system/daily-reboot']))))
    page.goto(url + '/system-settings?section=system')
    page.get_by_role('button', name='与当前电脑同步', exact=True).click()
    pw_api.expect(page.get_by_text('系统时间已与当前电脑同步', exact=True)).to_be_visible()
    assert len(saved) == 1 and 'local_time' not in saved[0]
    assert abs(saved[0]['epoch_ms'] - page.evaluate('Date.now()')) < 5000
    assert saved[0]['timezone'] == 'Asia/Shanghai'


def test_manual_time_conflict_tracks_external_timezone_and_retains_input(console):
    page, url, snapshots, _, _ = console
    state = snapshots['/api/system/daily-reboot']
    page.goto(url + '/system-settings?section=system')
    control = page.get_by_label('修改日期与时间', exact=True)
    target = '2026-10-20T12:34:56'
    control.fill(target)
    state.update(timezone='UTC', utc_offset_minutes=0)
    pw_api.expect(page.locator('.config-sync-notice')).to_contain_text('底层配置已变化', timeout=2500)
    pw_api.expect(control).to_have_value(target)
    pw_api.expect(page.get_by_role('button', name='应用时间', exact=True)).to_be_disabled()
    page.get_by_role('button', name='放弃修改并同步').click()
    pw_api.expect(page.locator('.config-sync-notice')).to_have_count(0)
    assert control.input_value() != target


def test_clock_read_unknown_disables_write_and_recovers_without_losing_draft(console):
    page, url, snapshots, _, _ = console
    state = snapshots['/api/system/daily-reboot']
    page.goto(url + '/system-settings?section=system')
    control = page.get_by_label('修改日期与时间', exact=True)
    control.fill('2026-10-20T12:34:56')
    state.update(time_error='读取校时状态超时')
    pw_api.expect(page.get_by_text('时间设置状态读取失败：读取校时状态超时', exact=True)).to_be_visible(timeout=2500)
    pw_api.expect(page.get_by_role('button', name='应用时间', exact=True)).to_be_disabled()
    state['time_error'] = None
    pw_api.expect(page.get_by_role('button', name='应用时间', exact=True)).to_be_enabled(timeout=2500)
    pw_api.expect(control).to_have_value('2026-10-20T12:34:56')


def test_failed_manual_time_write_keeps_draft_for_retry(console):
    page, url, _, _, _ = console
    page.route('**/api/system/time', lambda route: route.fulfill(
        status=500, content_type='application/json', body=json.dumps({'detail': '设置系统时间失败：RTC 错误'})))
    page.goto(url + '/system-settings?section=system')
    control = page.get_by_label('修改日期与时间', exact=True)
    control.fill('2026-10-20T12:34:56')
    page.get_by_role('button', name='应用时间', exact=True).click()
    pw_api.expect(page.get_by_text('设置系统时间失败：RTC 错误', exact=True)).to_be_visible()
    pw_api.expect(control).to_have_value('2026-10-20T12:34:56')
    pw_api.expect(page.get_by_role('button', name='应用时间', exact=True)).to_be_enabled()


def test_delayed_read_cannot_restore_old_values_after_a_save(console):
    page, url, snapshots, _, _ = console
    state = snapshots['/api/system/daily-reboot']
    page.goto(url + '/system-settings?section=system')
    minute = page.locator('.system-time-picker select').nth(1)
    pw_api.expect(minute).to_have_value('00')
    stale = deepcopy(state)
    held, saved = [], []

    def delayed(route):
        if route.request.method == 'PUT':
            payload = route.request.post_data_json
            state.update(time=payload['time'], enabled=payload['enabled'], configured_enabled=payload['enabled'])
            saved.append(payload)
            route.fulfill(content_type='application/json', body=json.dumps(state))
        elif not held:
            held.append(route)
        else:
            route.fallback()

    page.route('**/api/system/daily-reboot', delayed)
    page.wait_for_timeout(1200)
    assert held
    minute.select_option('01')
    page.get_by_role('button', name='保存重启设置', exact=True).click()
    pw_api.expect(page.get_by_text('已关闭每日定时重启', exact=True)).to_be_visible()
    assert saved == [{'enabled': False, 'time': '04:01'}]
    held[0].fulfill(content_type='application/json', body=json.dumps(stale))
    page.wait_for_timeout(250)
    pw_api.expect(minute).to_have_value('01')


def test_storage_policy_sync_is_lightweight_and_retains_dirty_values(console):
    page, url, snapshots, _, requests = console
    state = snapshots['/api/system/storage']
    page.goto(url + '/system-settings?section=storage')
    days = page.get_by_label('记录保留天数', exact=False)
    pw_api.expect(days).to_have_value('30')
    state.update(auto_cleanup=True, retention_days=9)
    pw_api.expect(days).to_have_value('9', timeout=2500)
    pw_api.expect(page.locator('.system-switch input')).to_be_checked()
    days.fill('17')
    state['retention_days'] = 5
    pw_api.expect(page.locator('.config-sync-notice')).to_contain_text('底层配置已变化', timeout=2500)
    pw_api.expect(days).to_have_value('17')
    pw_api.expect(page.get_by_role('button', name='保存存储策略')).to_be_disabled()
    assert sum(path == '/api/system/storage' for _, path in requests) >= 3


def test_failed_sync_marks_stale_values_and_retries_without_reloading(console):
    page, url, snapshots, _, _ = console
    page.goto(url + '/system-settings?section=storage')
    days = page.get_by_label('记录保留天数', exact=False)
    pw_api.expect(days).to_have_value('30')
    offline = [True]

    def flaky(route):
        if offline[0]:
            route.fulfill(status=503, content_type='application/json', body='{"detail":"temporarily offline"}')
        else:
            route.fallback()

    page.route('**/api/system/storage*', flaky)
    pw_api.expect(page.locator('.config-sync-notice')).to_contain_text('自动同步失败', timeout=2500)
    pw_api.expect(days).to_have_value('30')
    pw_api.expect(page.get_by_role('button', name='保存存储策略')).to_be_disabled()
    snapshots['/api/system/storage']['retention_days'] = 8
    offline[0] = False
    pw_api.expect(days).to_have_value('8', timeout=2500)
    pw_api.expect(page.locator('.config-sync-notice')).to_have_count(0)


@pytest.mark.parametrize('section,endpoint,label,value', [
    ('storage', '/api/system/storage', '记录保留天数', '30'),
    ('network', '/api/system/network', '名称', 'before-host'),
])
def test_first_read_failure_also_retries_automatically(console, section, endpoint, label, value):
    page, url, _, _, _ = console
    failures = [0]

    def fail_once(route):
        failures[0] += 1
        if failures[0] == 1:
            route.fulfill(status=503, content_type='application/json', body='{"detail":"initial read failed"}')
        else:
            route.fallback()

    page.route('**' + endpoint + '*', fail_once)
    page.goto(url + '/system-settings?section=' + section)
    pw_api.expect(page.get_by_label(label, exact=section == 'network')).to_have_value(value, timeout=4000)
    assert failures[0] >= 2


def test_save_rechecks_external_state_even_before_the_next_poll(console):
    page, url, snapshots, writes, _ = console
    page.goto(url + '/system-settings?section=storage')
    days = page.get_by_label('记录保留天数', exact=False)
    pw_api.expect(days).to_have_value('30')
    # Freeze periodic reads to exercise the gap before the next one-second tick.
    page.evaluate('for (let id = 1; id < 10000; id++) window.clearInterval(id)')
    days.fill('17')
    snapshots['/api/system/storage']['retention_days'] = 7
    page.get_by_role('button', name='保存存储策略').click()
    pw_api.expect(page.locator('.config-sync-notice')).to_contain_text('底层配置已变化')
    pw_api.expect(days).to_have_value('17')
    assert not writes


def test_hostname_and_network_fields_follow_same_connection_changes(console):
    page, url, snapshots, _, _ = console
    state = snapshots['/api/system/network']
    page.goto(url + '/system-settings?section=network')
    hostname = page.get_by_label('名称', exact=True)
    address = page.get_by_label('固定 IP 地址', exact=True)
    pw_api.expect(hostname).to_have_value('before-host')
    state['hostname'] = 'after-host'
    state['interfaces'][0].update(addresses=['192.0.2.20/24'], gateway='192.0.2.2', dns=['192.0.2.54'])
    pw_api.expect(hostname).to_have_value('after-host', timeout=2500)
    pw_api.expect(address).to_have_value('192.0.2.20')
    hostname.fill('my-unsaved-host')
    state['hostname'] = 'another-host'
    pw_api.expect(page.locator('.config-sync-notice')).to_contain_text('底层配置已变化', timeout=2500)
    pw_api.expect(hostname).to_have_value('my-unsaved-host')
    pw_api.expect(page.get_by_role('button', name='保存名称')).to_be_disabled()


def test_camera_external_file_updates_sync_without_discovery_or_apply(console):
    page, url, snapshots, _, requests = console
    page.goto(url + '/camera-settings')
    address = page.get_by_label('摄像头 IPv4', exact=True)
    pw_api.expect(address).to_have_value('192.0.2.50')
    snapshots['/api/system/camera']['configuration']['camera_ip'] = '192.0.2.60'
    pw_api.expect(address).to_have_value('192.0.2.60', timeout=3500)
    address.fill('192.0.2.70')
    snapshots['/api/system/camera']['configuration']['camera_ip'] = '192.0.2.80'
    pw_api.expect(page.locator('.config-sync-notice')).to_contain_text('底层配置已变化', timeout=2500)
    pw_api.expect(address).to_have_value('192.0.2.70')
    assert not any('discover' in path for _, path in requests)


@pytest.mark.parametrize('route,endpoint', [('/', '/api/apps'), ('/services', '/api/services')])
def test_application_and_service_autostart_follow_external_state(console, route, endpoint):
    page, url, snapshots, _, _ = console
    if endpoint == '/api/services':
        snapshots[endpoint][0]['scope'] = 'app'
    page.goto(url + route)
    checkbox = page.get_by_role('checkbox')
    pw_api.expect(checkbox).not_to_be_checked()
    snapshots[endpoint][0]['autostart'] = True
    pw_api.expect(checkbox).to_be_checked(timeout=2500)


def test_editor_and_application_integration_sync_files_and_protect_drafts(console):
    page, url, snapshots, _, _ = console
    page.goto(url + '/editor/demo')
    fps = page.locator('.gs-field').filter(has_text='全局处理 FPS').locator('input')
    pw_api.expect(fps).to_have_value('25')
    snapshots['/api/apps/demo/config']['global']['max_fps'] = 17
    pw_api.expect(fps).to_have_value('17', timeout=3000)
    fps.fill('19')
    snapshots['/api/apps/demo/config']['global']['max_fps'] = 21
    pw_api.expect(page.locator('.config-sync-notice')).to_contain_text('底层配置已变化', timeout=2500)
    pw_api.expect(fps).to_have_value('19')
    page.get_by_role('button', name='放弃修改并同步').click()
    pw_api.expect(fps).to_have_value('21')
    page.get_by_role('button', name='🔌 应用集成', exact=True).click()
    host = page.get_by_label('平台 WebSocket 地址', exact=False)
    pw_api.expect(host).to_have_value('before.example')
    snapshots['/api/apps/demo/ota-config']['platform_ws_host'] = 'after.example'
    pw_api.expect(host).to_have_value('after.example', timeout=2500)
    host.fill('unsaved.example')
    snapshots['/api/apps/demo/ota-config']['platform_ws_host'] = 'external.example'
    notice = page.get_by_role('dialog').locator('.config-sync-notice')
    pw_api.expect(notice).to_contain_text('底层配置已变化', timeout=2500)
    pw_api.expect(host).to_have_value('unsaved.example')
    pw_api.expect(page.get_by_role('button', name='保存到 demo', exact=True)).to_be_disabled()


def test_helmet_combined_mode_exposes_independent_width_height_and_bare_head_parameters(console):
    page, url, snapshots, _, _ = console
    catalog = json.loads((Path(__file__).resolve().parents[3] /
        'projects/modules/logic_crane_helmet/logic.json').read_text())
    properties = catalog['parameters']['properties']
    params = []
    for key, value in properties.items():
        kind = 'enum' if 'enum' in value else {'integer': 'int', 'number': 'float'}.get(value['type'], value['type'])
        params.append(dict(key=key, type=kind, label=value['title'], default=value['default'],
                           options=value.get('enum'), unit=value.get('x-unit'), help=value.get('description')))
    snapshots['/api/apps/demo/logics']['channel_logics'] = [dict(
        name='logic_crane_helmet', label=catalog['label'], params=params)]
    snapshots['/api/apps/demo/config']['channels'] = [dict(
        id=2, enable=True, logic='logic_crane_helmet', models=[],
        stream=dict(src_type='file', path='assets/isolated.mp4'),
        logic_parameters=dict(alarm_mode='missing_helmet', person_min_width=40, person_min_height=40), roi_zones=[])]
    page.goto(url + '/editor/demo')
    page.locator('.react-flow__node-logic').click()
    width = page.locator('.node-field').filter(has=page.locator('label').filter(
        has_text='未匹配安全帽的人员框最小宽度')).locator('input')
    height = page.locator('.node-field').filter(has=page.locator('label').filter(
        has_text='未匹配安全帽的人员框最小高度')).locator('input')
    pw_api.expect(width).to_have_value('40')
    pw_api.expect(height).to_have_value('40')
    pw_api.expect(page.locator('.node-field').filter(has_text='未匹配安全帽的人员框最小面积')).to_have_count(0)
    mode = page.locator('.node-field').filter(has=page.locator('label').filter(
        has_text='安全帽告警判断方式')).locator('select')
    pw_api.expect(mode.locator('option:checked')).to_have_text('裸头优先 + 人员宽高过滤')
    head = page.locator('.node-field').filter(has=page.locator('label').filter(
        has_text='未戴帽头部最低置信度')).locator('input')
    pw_api.expect(head).to_be_visible()
    pw_api.expect(head).to_have_value('0.3')
    width.fill('35')
    height.fill('75')
    page.wait_for_timeout(1200)
    pw_api.expect(width).to_have_value('35')
    pw_api.expect(height).to_have_value('75')
    pw_api.expect(head).to_have_value('0.3')


def open_business_roi(console, inference_roi=None, polygon=None, zones=None):
    import base64
    page, url, snapshots, _, _ = console
    image = '<svg xmlns="http://www.w3.org/2000/svg" width="1920" height="1080"><rect width="1920" height="1080" fill="#7ca0c4"/><path d="M960 0V1080M0 540H1920" stroke="#446688" stroke-width="8"/></svg>'
    snapshots['/api/apps/demo/snapshot'] = dict(width=1920, height=1080,
        image='data:image/svg+xml;base64,' + base64.b64encode(image.encode()).decode())
    snapshots['/api/apps/demo/config']['channels'] = [dict(
        id=0, enable=True, models=[], stream=dict(src_type='file', url='assets/isolated.mp4'),
        roi_zones=zones if zones is not None else [dict(name='作业区域', polygon=polygon or [[.25,.25],[.75,.25],[.75,.75],[.25,.75]])],
        **({'inference_roi': inference_roi} if inference_roi else {}))]
    page.add_init_script('''window.roiLabels = []; window.roiLabelFrames = {};
      const clear = CanvasRenderingContext2D.prototype.clearRect;
      CanvasRenderingContext2D.prototype.clearRect = function(...args) {
        const view = this.canvas.getAttribute('aria-label');
        window.roiLabelFrames[view] = [];
        return clear.call(this, ...args);
      };
      const fill = CanvasRenderingContext2D.prototype.fillText;
      CanvasRenderingContext2D.prototype.fillText = function(text, ...args) {
        const view = this.canvas.getAttribute('aria-label');
        window.roiLabels.push([view, text]);
        if (/^\\d+(?: \\(|\\. )/.test(text)) {
          const scale = parseFloat(this.font) / 12;
          (window.roiLabelFrames[view] ||= []).push({text,
            x:args[0]-3*scale,y:args[1]-14*scale,
            width:this.measureText(text).width+6*scale,height:19*scale});
        }
        if (window.roiLabels.length > 2000) window.roiLabels.shift();
        return fill.call(this, text, ...args);
      };''')
    page.goto(url + '/editor/demo')
    button = page.get_by_role('button', name='编辑业务 ROI', exact=True)
    if page.viewport_size['width'] <= 900:
        # 窄屏用节点事件打开被编辑器其他面板遮住的弹窗，单独验证 ROI 布局。
        button.dispatch_event('click')
    else:
        button.click()
    dialog = page.get_by_role('dialog', name='业务 ROI 绘制', exact=True)
    pw_api.expect(dialog.get_by_role('button', name='＋ 新增区域', exact=True)).to_be_enabled()
    return page, dialog


def roi_point(page, canvas, x, y, click=True):
    box = canvas.bounding_box()
    assert box
    point = (box['x'] + box['width'] * x, box['y'] + box['height'] * y)
    if click:
        page.mouse.click(*point)
    return point


def last_roi_point(page, view, point=1):
    import re
    label = page.evaluate("""([view, point]) => [...window.roiLabels].reverse().find(
      ([name, text]) => name === view && text.startsWith(point + ' ('))?.[1]""", [view, point])
    assert label
    return tuple(map(int, re.search(r'\((\d+), (\d+)\)', label).groups()))


def roi_dimensions(dialog):
    return dialog.evaluate("""e => [e, ...e.querySelectorAll('canvas'), e.querySelector('.business-roi-main-scroll')]
      .map(node => { const r = node.getBoundingClientRect(); return [r.width, r.height]; })""")


def assert_roi_dimensions_unchanged(dialog, expected):
    # Transforms can introduce tiny float differences when the origin moves.
    # Compare raw dimensions with an absolute tolerance, avoiding rounding bins.
    actual = roi_dimensions(dialog)
    assert len(actual) == len(expected)
    for size, baseline in zip(actual, expected):
        assert size == pytest.approx(baseline, rel=0, abs=.001)


def test_business_roi_dual_preview_maps_source_and_fixed_business_coordinates(console):
    page, dialog = open_business_roi(console)
    main = dialog.get_by_label('原图 ROI 绘制画布', exact=True)
    preview = dialog.get_by_label('640×640 业务 ROI 预览', exact=True)
    pw_api.expect(dialog.locator('canvas')).to_have_count(2)
    pw_api.expect(dialog.get_by_label('绘制缩放', exact=True)).to_have_count(0)
    pw_api.expect(dialog.get_by_role('table')).to_have_count(0)
    assert preview.get_attribute('width') == preview.get_attribute('height') == '640'
    assert last_roi_point(page, '原图 ROI 绘制画布') == (480, 270)
    assert last_roi_point(page, '640×640 业务 ROI 预览') == (160, 160)
    before = roi_dimensions(dialog)
    start = roi_point(page, main, .25, .25, click=False)
    # 鼠标事件使用整数屏幕像素；画布变窄时，一个显示像素会对应多个业务像素。
    end = tuple(round(value) for value in roi_point(page, main, .30, .30, click=False))
    box = main.bounding_box()
    expected = (round((end[0] - box['x']) / box['width'] * 640),
                round((end[1] - box['y']) / box['height'] * 640))
    page.mouse.move(*start); page.mouse.down(); page.mouse.move(*end, steps=4); page.mouse.up()
    moved = last_roi_point(page, '640×640 业务 ROI 预览')
    assert abs(moved[0] - expected[0]) <= 1 and abs(moved[1] - expected[1]) <= 1
    assert_roi_dimensions_unchanged(dialog, before)
    dialog.get_by_role('button', name='保存', exact=True).click()
    page.get_by_role('button', name='编辑业务 ROI', exact=True).click()
    pw_api.expect(page.get_by_role('dialog').get_by_role('button', name='＋ 新增区域', exact=True)).to_be_enabled()
    assert last_roi_point(page, '640×640 业务 ROI 预览') == moved


@pytest.mark.parametrize('width', [1500, 390])
def test_business_roi_drawing_keeps_window_and_images_fixed_without_extra_black_padding(console, width):
    import math
    page = console[0]
    page.set_viewport_size({'width': width, 'height': 1100})
    page, dialog = open_business_roi(console)
    main = dialog.get_by_label('原图 ROI 绘制画布', exact=True)
    baseline = roi_dimensions(dialog)
    assert baseline[1][1] == baseline[3][1]  # 大图容器不为侧栏高度填黑边。
    assert dialog.evaluate('e => e.scrollWidth <= e.clientWidth + 1')
    assert main.evaluate("e => [...e.getContext('2d').getImageData(10,10,1,1).data]") == [124,160,196,255]
    dialog.get_by_role('button', name='＋ 新增区域', exact=True).click()
    dialog.locator('.roi-body').evaluate('e => { e.scrollTop = 0; }')
    for index in range(8):
        angle = index * math.pi / 4
        roi_point(page, main, .25 + .15 * math.cos(angle), .25 + .15 * math.sin(angle))
        assert_roi_dimensions_unchanged(dialog, baseline)
    pw_api.expect(dialog.get_by_role('button', name='📷 抓取当前帧', exact=True)).to_be_disabled()
    page.mouse.click(*roi_point(page, main, .35, .35, click=False), button='right')
    pw_api.expect(dialog.locator('.roi-draw-hint')).to_contain_text('已选 7 个顶点')
    assert_roi_dimensions_unchanged(dialog, baseline)
    dialog.get_by_role('button', name='完成此区域', exact=True).click()
    assert_roi_dimensions_unchanged(dialog, baseline)
    # 浏览器尺寸变化也不在同一次编辑中重新缩放窗口或画面。
    page.set_viewport_size({'width': width + 100, 'height': 1000})
    assert_roi_dimensions_unchanged(dialog, baseline)


def test_business_roi_preview_keeps_image_and_inference_boundary_restrictions(console):
    page, dialog = open_business_roi(console, dict(mode='full_frame_roi_filter', resize_mode='stretch',
        polygon=[[.2,.2],[.8,.2],[.8,.8],[.2,.8]]))
    preview = dialog.get_by_label('640×640 业务 ROI 预览', exact=True)
    pixel = preview.evaluate("e => [...e.getContext('2d').getImageData(320,320,1,1).data]")
    assert pixel[3] == 255 and pixel[2] > 0
    main = dialog.get_by_label('原图 ROI 绘制画布', exact=True)
    baseline = roi_dimensions(dialog)
    dialog.get_by_role('button', name='＋ 新增区域', exact=True).click()
    roi_point(page, main, .1, .1)
    pw_api.expect(dialog.get_by_role('button', name='完成此区域', exact=True)).to_be_disabled()
    pw_api.expect(dialog.locator('.roi-draft-status')).to_have_text('正在新增区域：已选 0 个顶点')
    assert_roi_dimensions_unchanged(dialog, baseline)


def assert_readable_roi_labels(page, dialog):
    for canvas in dialog.locator('canvas').all():
        view = canvas.get_attribute('aria-label')
        labels = page.evaluate('view => window.roiLabelFrames[view]', view)
        assert labels, view
        width, height = int(canvas.get_attribute('width')), int(canvas.get_attribute('height'))
        for index, label in enumerate(labels):
            # Canvas 的 font 属性会舍入小数像素，反推边框时允许亚像素误差。
            assert label['x'] >= -.01 and label['y'] >= -.01
            assert label['x'] + label['width'] <= width + .01
            assert label['y'] + label['height'] <= height + .01
            for other in labels[:index]:
                assert (label['x'] >= other['x'] + other['width'] or
                        other['x'] >= label['x'] + label['width'] or
                        label['y'] >= other['y'] + other['height'] or
                        other['y'] >= label['y'] + label['height']), (view, label, other)


@pytest.mark.parametrize('count,center,radius', [(16,.5,.025),(64,.04,.025)])
def test_dense_roi_labels_do_not_overlap_and_hover_reveals_requested_vertex(console, count, center, radius):
    import math
    polygon = [[center + radius * math.cos(2*math.pi*i/count),
                center + radius * math.sin(2*math.pi*i/count)] for i in range(count)]
    page, dialog = open_business_roi(console, polygon=polygon)
    baseline = roi_dimensions(dialog)
    assert_readable_roi_labels(page, dialog)
    main = dialog.get_by_label('原图 ROI 绘制画布', exact=True)
    # 接近画面边缘且顶点很密，也应优先显示离鼠标最近的点。
    targets = [roi_point(page, main, *point, click=False) for point in polygon]
    index = min(range(count), key=lambda i: sum(abs(round(value)-value) for value in targets[i]))
    page.mouse.move(*(round(value) for value in targets[index]))
    page.wait_for_timeout(150)
    for view in ('原图 ROI 绘制画布','640×640 业务 ROI 预览'):
        labels = page.evaluate('view => window.roiLabelFrames[view]', view)
        assert any(label['text'].startswith(f'{index+1} (') for label in labels), (view,index,labels)
    assert_readable_roi_labels(page, dialog)
    assert_roi_dimensions_unchanged(dialog, baseline)


def test_dense_roi_draft_labels_stay_readable_while_adding_points(console):
    import math
    page, dialog = open_business_roi(console)
    main = dialog.get_by_label('原图 ROI 绘制画布', exact=True)
    baseline = roi_dimensions(dialog)
    dialog.get_by_role('button', name='＋ 新增区域', exact=True).click()
    for index in range(12):
        angle = 2*math.pi*index/12
        roi_point(page, main, .5+.05*math.cos(angle), .5+.05*math.sin(angle))
        assert_readable_roi_labels(page, dialog)
        assert_roi_dimensions_unchanged(dialog, baseline)


def assert_roi_content_fits_without_scrolling(dialog):
    # 检查真实内容尺寸，防止只隐藏滚动条却把区域、提示或按钮裁掉。
    overflow = dialog.evaluate('''root => [root, ...root.querySelectorAll('*')]
      .filter(e => e.tagName !== 'INPUT' && e.clientWidth > 0 && e.clientHeight > 0)
      .filter(e => e.scrollWidth > e.clientWidth + 1 || e.scrollHeight > e.clientHeight + 1
        || ['auto', 'scroll'].includes(getComputedStyle(e).overflowX)
        || ['auto', 'scroll'].includes(getComputedStyle(e).overflowY))
      .map(e => ({className:e.className, width:[e.clientWidth,e.scrollWidth],
        height:[e.clientHeight,e.scrollHeight]}))''')
    assert not overflow, overflow
    bounds = dialog.bounding_box()
    for item in dialog.locator('canvas, button, .roi-zone-item, .roi-name-error').all():
        rect = item.bounding_box()
        assert rect and rect['x'] >= bounds['x'] and rect['y'] >= bounds['y']
        assert rect['x'] + rect['width'] <= bounds['x'] + bounds['width'] + 1
        assert rect['y'] + rect['height'] <= bounds['y'] + bounds['height'] + 1


@pytest.mark.parametrize('width,height', [(1500,1100),(1366,768),(390,844)])
def test_business_roi_all_controls_fit_without_scrollbars_with_many_zones_and_errors(console, width, height):
    console[0].set_viewport_size({'width':width, 'height':height})
    zones = [dict(name=f'区域{i+1}', polygon=[[.25,.25],[.75,.25],[.75,.75],[.25,.75]])
             for i in range(12)]
    inference = dict(mode='full_frame_roi_filter', resize_mode='stretch',
        polygon=[[.1,.1],[.9,.1],[.9,.9],[.1,.9]]) if width == 1366 else None
    page, dialog = open_business_roi(console, zones=zones, inference_roi=inference)
    baseline = roi_dimensions(dialog)
    pw_api.expect(dialog.get_by_role('complementary', name='ROI 区域列表')
                  .get_by_role('button', name='＋ 新增区域', exact=True)).to_be_enabled()
    pw_api.expect(dialog.locator('.business-roi-content').locator('button, .business-roi-tools, .roi-name-error')).to_have_count(0)
    pw_api.expect(dialog.get_by_role('complementary').locator('.business-roi-tools')).to_have_count(1)
    if width > 900:
        sidebar = dialog.get_by_role('complementary', name='ROI 区域列表').bounding_box()
        main = dialog.get_by_label('原图 ROI 绘制画布', exact=True).bounding_box()
        preview = dialog.get_by_label('640×640 业务 ROI 预览', exact=True).bounding_box()
        assert preview['width'] >= 350  # 列表移回侧栏时仍保留放大的业务预览。
        assert sidebar['x'] >= preview['x'] + preview['width']
        assert sidebar['y'] <= main['y']
        assert dialog.get_by_role('complementary').locator('.roi-zone-item').count() == 12
    assert_roi_content_fits_without_scrolling(dialog)
    dialog.get_by_role('button', name='＋ 新增区域', exact=True).click()
    main = dialog.get_by_label('原图 ROI 绘制画布', exact=True)
    for x in [.2,.4,.6]:
        roi_point(page, main, x, .5)
    dialog.locator('.roi-zone-input').nth(1).fill('区域1')
    pw_api.expect(dialog.locator('.business-roi-tools')).to_contain_text('ROI 区域名称不能重复')
    pw_api.expect(dialog.locator('.roi-draw-hint')).to_contain_text('当前边界自相交、面积无效')
    pw_api.expect(dialog.get_by_role('complementary').get_by_role('button', name='完成此区域', exact=True)).to_be_visible()
    pw_api.expect(dialog.get_by_role('complementary').get_by_role('button', name='取消', exact=True)).to_be_visible()
    assert_roi_dimensions_unchanged(dialog, baseline)
    assert_roi_content_fits_without_scrolling(dialog)


@pytest.mark.parametrize('width,height', [(1215,753),(1500,1100)])
def test_business_roi_balances_images_and_removes_empty_space_without_resizing_during_drawing(console, width, height):
    console[0].set_viewport_size({'width':width, 'height':height})
    page, dialog = open_business_roi(console)
    main = dialog.get_by_label('原图 ROI 绘制画布', exact=True)
    preview = dialog.get_by_label('640×640 业务 ROI 预览', exact=True)
    main_box, preview_box = main.bounding_box(), preview.bounding_box()
    window_box = dialog.bounding_box()
    unscaled = dialog.evaluate('e => [e.offsetWidth, e.offsetHeight]')
    scale = window_box['width'] / unscaled[0]
    assert 1 <= scale <= 1.121
    assert abs(window_box['height'] / unscaled[1] - scale) < .001
    if width == 1500:
        assert abs(scale - 1.12) < .001
    assert window_box['x'] >= 3 and window_box['y'] >= 3
    assert window_box['x'] + window_box['width'] <= width - 3
    assert window_box['y'] + window_box['height'] <= height - 3
    content = dialog.locator('.business-roi-content').bounding_box()
    assert abs((main_box['x'] - content['x']) / scale - 12) < 1
    assert main_box['width'] >= preview_box['width'] * 1.7
    # CSS transform 的边界按浮点像素返回，保留微小舍入容差。
    assert abs(main_box['height'] - preview_box['height']) <= scale + .01
    assert window_box['height'] <= 600 * scale
    body = dialog.locator('.roi-body').bounding_box()
    bottom = max(main_box['y'] + main_box['height'], preview_box['y'] + preview_box['height'])
    assert body['y'] + body['height'] - bottom < 60 * scale
    assert_roi_content_fits_without_scrolling(dialog)
    baseline = roi_dimensions(dialog)
    sidebar = dialog.get_by_role('complementary', name='ROI 区域列表')
    pw_api.expect(sidebar.locator('.roi-sidebar-title')).to_have_text('已绘制区域（1）')
    sidebar.get_by_role('button', name='＋ 新增区域', exact=True).click()
    pw_api.expect(sidebar.locator('.roi-draft-status')).to_have_text('正在新增区域：已选 0 个顶点')
    pw_api.expect(sidebar.locator('.roi-zone-pts')).to_have_text('4点')
    for x,y in [(.2,.2),(.65,.3),(.3,.7)]:
        roi_point(page, main, x, y)
        assert_roi_dimensions_unchanged(dialog, baseline)
    pw_api.expect(sidebar.locator('.roi-draft-status')).to_have_text('正在新增区域：已选 3 个顶点')
    sidebar.get_by_role('button', name='完成此区域', exact=True).click()
    pw_api.expect(sidebar.locator('.roi-sidebar-title')).to_have_text('已绘制区域（2）')
    assert_roi_dimensions_unchanged(dialog, baseline)
    sidebar.get_by_role('button', name='＋ 新增区域', exact=True).click()
    sidebar.get_by_role('button', name='取消', exact=True).click()
    assert_roi_dimensions_unchanged(dialog, baseline)
    assert_roi_content_fits_without_scrolling(dialog)
