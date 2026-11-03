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
    assert page.get_by_role('combobox').count() == 0
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
