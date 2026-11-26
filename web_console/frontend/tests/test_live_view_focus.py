"""Real H264 playback: local tile focus preserves pixels, connection and controls."""
import subprocess
import os

import pytest
from test_live_view_playback import playback  # noqa: F401 -- shared real-browser fixture


@pytest.fixture(scope='module')
def fragments(tmp_path_factory):
    path = tmp_path_factory.mktemp('focus-video') / 'colors.mp4'
    subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
        'color=red:size=320x180:rate=10', '-vf',
        'drawbox=x=160:y=0:w=160:h=90:color=lime:t=fill,'
        'drawbox=x=0:y=90:w=160:h=90:color=blue:t=fill,'
        'drawbox=x=160:y=90:w=160:h=90:color=yellow:t=fill',
        '-t', '60', '-an', '-c:v', 'libx264', '-preset', 'ultrafast', '-tune', 'zerolatency',
        '-g', '10', '-pix_fmt', 'yuv420p', '-movflags', 'empty_moov+default_base_moof',
        '-frag_duration', '100000', str(path)], check=True, timeout=30)
    data = path.read_bytes()
    parts, current, offset = [], bytearray(), 0
    while offset < len(data):
        size = int.from_bytes(data[offset:offset + 4], 'big')
        assert size >= 8
        if data[offset + 4:offset + 8] == b'moof' and current:
            parts.append(bytes(current))
            current.clear()
        current.extend(data[offset:offset + size])
        offset += size
    if current:
        parts.append(bytes(current))
    return parts, .1


def configure(page, state, ids=(9, 2, 14, 6)):
    state['config'] = {'global': {'enable_rtsp': 1, 'disp_width': 320, 'disp_height': 180,
        'tile_rows': 2, 'tile_cols': 2}, 'channels': [
        {'id': channel, 'stream': {'src_type': 'rtsp', 'url': 'rtsp://test/source'}} for channel in ids]}
    page.reload()
    page.wait_for_function("document.querySelector('video')?.currentTime > 1")
    page.get_by_role('button', name='单独查看通道 2', exact=True).wait_for()
    page.evaluate('window.originalVideo = document.querySelector("video")')
    return state['requests']


def assert_color(page, expected):
    crop = page.locator('.live-view-video-crop')
    png = crop.screenshot()
    width, height = int.from_bytes(png[16:20], 'big'), int.from_bytes(png[20:24], 'big')
    pixels = subprocess.run(['ffmpeg', '-hide_banner', '-loglevel', 'error', '-i', 'pipe:0',
        '-f', 'rawvideo', '-pix_fmt', 'rgb24', 'pipe:1'], input=png, capture_output=True,
        check=True, timeout=10).stdout
    for x, y in [(0.2, 0.3), (0.8, 0.3), (0.2, 0.7), (0.8, 0.7)]:
        offset = (int(height*y)*width + int(width*x))*3
        pixel = tuple(pixels[offset:offset+3])
        # H264/YUV and the desktop color profile need not preserve exact RGB.
        assert all(a > 180 if b else a < 35 for a,b in zip(pixel, expected)), (pixel, expected)


def test_focus_pixels_resize_fullscreen_and_connection(playback, tmp_path):
    page, _, state = playback
    requests = configure(page, state)
    assert page.get_by_label('点击画面后的显示方式').input_value() == 'independent'
    page.get_by_label('点击画面后的显示方式').select_option('zoom')
    assert page.get_by_role('combobox').count() == 1
    assert page.locator('.live-view-channel-toolbar').count() == 0
    page.get_by_role('button', name='单独查看通道 9', exact=True).click()
    page.wait_for_function("document.querySelector('.live-view-video-crop').dataset.focusedChannel === '9'")
    assert_color(page, (0, 0, 255))
    page.set_viewport_size({'width': 1000, 'height': 850})
    assert_color(page, (0, 0, 255))
    # Chromium 91 requires a desktop session for native fullscreen.
    if os.environ.get('CHROMIUM_HEADED') == '1':
        page.get_by_role('button', name='全屏', exact=True).click()
        page.wait_for_function('!!document.fullscreenElement')
        assert_color(page, (0, 0, 255))
        page.get_by_role('button', name='点击画面返回拼接', exact=True).click()
        assert page.evaluate('!!document.fullscreenElement')
        page.get_by_role('button', name='单独查看通道 9', exact=True).click()
        assert_color(page, (0, 0, 255))
        page.screenshot(path=str(tmp_path/'focused-fullscreen.png'))
        page.get_by_role('button', name='退出全屏', exact=True).click()
    for channel, color in [(2, (255,0,0)), (6, (0,255,0)), (14, (255,255,0))]:
        page.get_by_role('button', name='点击画面返回拼接', exact=True).click()
        page.get_by_role('button', name=f'单独查看通道 {channel}', exact=True).click()
        assert_color(page, color)
    page.get_by_role('button', name='点击画面返回拼接', exact=True).click()
    assert page.get_by_role('button', name='单独查看通道 9', exact=True).is_visible()
    page.screenshot(path=str(tmp_path/'grid.png'))
    page.get_by_role('button', name='单独查看通道 6', exact=True).focus()
    page.keyboard.press('Enter')
    assert_color(page, (0,255,0))
    page.get_by_role('button', name='点击画面返回拼接', exact=True).focus()
    page.keyboard.press('Enter')
    assert page.locator('.live-view-video-crop').get_attribute('data-focused-channel') == 'grid'
    assert page.evaluate('document.querySelector("video") === originalVideo')
    assert state['requests'] == requests


def test_empty_cells_and_independent_viewers(playback):
    page, _, state = playback
    requests = configure(page, state, (9,2,6))
    bounds = page.locator('.live-view-tile-targets').bounding_box()
    page.mouse.click(bounds['x'] + bounds['width']*.75, bounds['y'] + bounds['height']*.75)
    assert page.locator('.live-view-video-crop').get_attribute('data-focused-channel') == 'grid'
    # Letterboxing also has no channel target.
    frame = page.locator('.live-view-video-frame').bounding_box()
    page.mouse.click(frame['x'] + 2, frame['y'] + frame['height'] - 2)
    assert page.locator('.live-view-video-crop').get_attribute('data-focused-channel') == 'grid'
    other = page.context.new_page()
    other.route_web_socket('**/ws/logs/**', lambda socket: None)
    try:
        other.goto(page.url)
        other.wait_for_function("document.querySelector('video')?.currentTime > 1")
        page.get_by_role('button', name='单独查看通道 9', exact=True).click()
        assert_color(page, (0,0,255))
        assert other.locator('.live-view-video-crop').get_attribute('data-focused-channel') == 'grid'
        other.get_by_role('button', name='单独查看通道 2', exact=True).click()
        assert_color(other, (255,0,0))
        assert page.locator('.live-view-video-crop').get_attribute('data-focused-channel') == '9'
        assert state['requests'] == requests + 1
    finally:
        other.close()


def test_file_timeline_and_recovery_preserve_focus(playback):
    page, _, state = playback
    state['sources'] = [dict(owner_channel_id=9, channel_ids=[9], location='/test/sample.mp4',
        available=True, seekable=True, playing=True, ended=False, position_ms=1000, duration_ms=60000)]
    requests = configure(page, state)
    seeks = []
    def seek(route):
        seeks.append(route.request.post_data_json)
        route.fulfill(json={'ok': True, 'channel_id': 9, 'position_ms': seeks[-1]['position_ms']})
    page.route('**/file-playback/9/seek', seek)
    page.get_by_role('button', name='单独查看通道 9', exact=True).hover()
    slider = page.get_by_role('slider')
    slider.wait_for(state='visible')
    slider.click()
    assert seeks
    assert page.locator('.live-view-video-crop').get_attribute('data-focused-channel') == 'grid'
    page.get_by_role('button', name='单独查看通道 9', exact=True).click(position={'x':30,'y':30})
    page.get_by_role('button', name='点击画面返回拼接', exact=True).hover()
    slider.wait_for(state='visible')
    assert slider.bounding_box()['width'] > page.locator('.live-view-video-crop').bounding_box()['width']*.8
    slider.click()
    assert page.locator('.live-view-video-crop').get_attribute('data-focused-channel') == '9'
    state['drop'] = True
    page.wait_for_function("!document.querySelector('.live-view-video-snapshot').hidden", timeout=10000)
    assert page.locator('.live-view-video-crop').get_attribute('data-focused-channel') == '9'
    page.wait_for_function('''() => document.querySelector('video') !== originalVideo &&
        document.querySelector('video').currentTime > 1 && document.querySelector('.live-view-video-snapshot').hidden''', timeout=15000)
    assert_color(page, (0,0,255))
    assert state['requests'] == requests + 1


def test_file_pause_resume_and_paused_seek_keep_preview_connected(playback, tmp_path):
    page, _, state = playback
    source = dict(owner_channel_id=9, channel_ids=[9], location='/test/sample.mp4',
        available=True, seekable=True, playing=True, paused=False, pause_requested=False,
        busy=False, ended=False, position_ms=1000, duration_ms=60000)
    state['sources'] = [source]
    requests = configure(page, state)
    commands = []
    def pause(route):
        value = route.request.post_data_json['paused']
        commands.append(value)
        source.update(paused=value, pause_requested=value, playing=not value, busy=False)
        route.fulfill(json={'ok': True, 'busy': True, 'pause_requested': value})
    def seek(route):
        source['position_ms'] = route.request.post_data_json['position_ms']
        route.fulfill(json={'ok': True, 'busy': True, 'position_ms': source['position_ms']})
    page.route('**/file-playback/9/pause', pause)
    page.route('**/file-playback/9/seek', seek)
    page.get_by_role('button', name='单独查看通道 9', exact=True).hover()
    pause_button = page.get_by_role('button', name='通道 9 暂停播放和分析', exact=True)
    pause_button.wait_for(state='visible')
    button_box, slider_box = pause_button.bounding_box(), page.get_by_role('slider').bounding_box()
    assert button_box['x'] + button_box['width'] <= slider_box['x']
    assert abs(button_box['y'] + button_box['height']/2 - slider_box['y'] - slider_box['height']/2) < 2
    assert pause_button.inner_text() == '' and pause_button.locator('svg rect').count() == 2
    page.locator('.live-view-file-timeline').screenshot(path=str(tmp_path / 'file-controls.png'))
    # 模拟已经积压的预览；文件操作应主动追到新画面，复用原来的连接。
    page.evaluate('''() => {
        const v = document.querySelector('video');
        v.currentTime = v.buffered.end(v.buffered.length - 1) - .45;
    }''')
    page.wait_for_function('!document.querySelector("video").seeking')
    pause_button.click()
    resume = page.get_by_role('button', name='通道 9 继续播放和分析', exact=True)
    from playwright.sync_api import expect
    expect(resume).to_be_enabled(timeout=600)
    assert resume.locator('svg path').count() == 1
    # 浏览器也可能在点击前就自动追上；验证剩余延迟，而不是强制要求再跳一次。
    page.wait_for_function('''() => {
        const v = document.querySelector('video');
        return !v.seeking && v.buffered.end(v.buffered.length - 1) - v.currentTime <= .35;
    }''', timeout=1000)
    expect(page.locator('.live-view-file-meta')).to_contain_text('已暂停')
    page.get_by_role('slider').click()
    expect(resume).to_be_enabled()
    assert source['paused'] is True
    resume.click()
    expect(page.get_by_role('button', name='通道 9 暂停播放和分析', exact=True)).to_be_enabled()
    assert commands == [True, False]
    assert state['requests'] == requests
    page.wait_for_function('document.querySelector("video") === originalVideo && !originalVideo.paused && !originalVideo.seeking', timeout=1500)
    # 实时输入没有文件时间条，也不会出现暂停分析按钮。
    page.get_by_role('button', name='单独查看通道 2', exact=True).hover()
    expect(page.get_by_role('button', name='通道 2 暂停播放和分析', exact=True)).to_have_count(0)


def test_hidden_tiles_and_preview_preferences(playback):
    page, _, state = playback
    page.route('**/stream/capabilities', lambda route: route.fulfill(json={
        'channel_preview':True}))
    configure(page, state)
    target = page.get_by_role('button', name='单独查看通道 2', exact=True)
    target.hover()
    style = target.evaluate('(e) => { const s = getComputedStyle(e); return [s.backgroundColor, s.boxShadow, getComputedStyle(e.querySelector("span")).opacity] }')
    assert style == ['rgba(0, 0, 0, 0)', 'none', '0']
    from playwright.sync_api import expect
    expect(page.get_by_label('独立预览分辨率')).to_have_count(0)
    target.click()
    page.wait_for_function("document.querySelector('video')?.currentTime > 1")
    assert state['queries'][-1]['channel_id'] == ['2']
    assert 'preview_height' not in state['queries'][-1]
    assert page.locator('video').evaluate('(v) => [v.videoWidth, v.videoHeight]') == [320, 180]
    # Returning to local zoom restores the mosaic stream, preserving focus.
    page.get_by_label('点击画面后的显示方式').select_option('zoom')
    page.wait_for_function("document.querySelector('video')?.currentTime > 1")
    assert 'channel_id' not in state['queries'][-1]
    assert_color(page, (255,0,0))
    assert page.locator('.live-view-video-crop').get_attribute('data-focused-channel') == '2'
    page.reload()
    page.get_by_label('点击画面后的显示方式').wait_for()
    assert page.get_by_label('点击画面后的显示方式').input_value() == 'zoom'
    page.get_by_label('点击画面后的显示方式').select_option('independent')
    expect(page.get_by_label('独立预览分辨率')).to_have_count(0)


@pytest.mark.parametrize('rows,columns', [(2, 2), (1, 2), (2, 1), (1, 1)])
def test_single_stream_focus_depends_on_grid_size(playback, rows, columns):
    page, _, state = playback
    state['config'] = {'global': {'enable_rtsp': 1, 'disp_width': 320, 'disp_height': 180,
        'tile_rows': rows, 'tile_cols': columns}, 'channels': [
        {'id': 0, 'enable': False, 'stream': {'src_type': 'file', 'url': '/disabled.mp4'}},
        {'id': 9, 'stream': {'src_type': 'rtsp', 'url': 'rtsp://test/source'}}]}
    page.reload()
    page.wait_for_function("document.querySelector('video')?.currentTime > 1")
    page.get_by_label('点击画面后的显示方式').select_option('zoom')
    target = page.get_by_role('button', name='单独查看通道 9', exact=True)
    assert page.get_by_role('button', name='单独查看通道 0', exact=True).count() == 0
    if rows == columns == 1:
        assert target.count() == 0
        return
    target.wait_for()
    before = state['requests']
    page.evaluate('window.originalVideo = document.querySelector("video")')
    target.click()
    page.wait_for_function("document.querySelector('.live-view-video-crop').dataset.focusedChannel === '9'")
    if rows == columns == 2:
        assert_color(page, (255, 0, 0))
    page.get_by_role('button', name='点击画面返回拼接', exact=True).click()
    target.wait_for()
    assert page.evaluate('document.querySelector("video") === originalVideo')
    assert state['requests'] == before
