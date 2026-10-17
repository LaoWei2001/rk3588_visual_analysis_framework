"""Browser regression against real dataset HTTP routes and an offline app.

Build the frontend first. Requires pytest, Python Playwright and Chromium;
run with PYTHONPATH=web_console/backend pytest -q <this file>.
"""
import json
import os
from pathlib import Path
import socket
import threading
import time

import pytest
import uvicorn
from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from routers import dataset
from services.dataset_manager import DatasetManager

playwright = pytest.importorskip('playwright.sync_api')
FRONTEND = Path(__file__).resolve().parents[1]


def test_capture_interval_minimum_and_fractional_value_create_edit_and_restore(console):
    url, manager = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                     headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context()
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        page.goto(url + '/dataset')
        page.get_by_role('button', name='＋ 新建采集任务', exact=True).click()
        interval = page.get_by_label('抓图间隔（秒）', exact=True)
        expect(interval).to_have_value('2')
        interval.fill('0.299')
        assert interval.evaluate('(input) => input.validity.rangeUnderflow')
        assert manager.rows() == []
        response = page.request.put(url + '/api/apps/demo/dataset/channels/0', data={
            'rules': [{'id': 'r', 'condition': {'class': 'person', 'min_confidence': .3, 'count': {'op': '>=', 'value': 1}}}],
            'interval_sec': .299})
        assert response.status == 422 and manager.rows() == []
        interval.fill('0.3')
        page.get_by_role('button', name='创建任务', exact=True).click()
        expect(page.locator('.dataset-config-panel')).to_have_count(0)
        assert json.loads(manager.rows()[0]['config'])['interval_sec'] == .3
        logic = page.locator('.dataset-task').get_by_role('region', name='取图逻辑', exact=True)
        expect(logic).to_contain_text('至少间隔 0.3 秒')
        page.reload()
        expect(logic).to_contain_text('至少间隔 0.3 秒')
        page.get_by_role('button', name='编辑配置', exact=True).click()
        expect(interval).to_have_value('0.3')
        interval.fill('0.35')
        page.get_by_role('button', name='保存修改', exact=True).click()
        expect(page.locator('.dataset-config-panel')).to_have_count(0)
        expect(logic).to_contain_text('至少间隔 0.35 秒')
        assert json.loads(manager.rows()[0]['config'])['interval_sec'] == .35
        browser.close()


@pytest.mark.parametrize('relation', ['center_inside', 'overlap', 'near'])
@pytest.mark.parametrize('custom_values', [False, True])
def test_create_multiple_category_task_with_default_matching_thresholds(console, relation, custom_values):
    url, manager = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                     headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context()
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        page.goto(url + '/dataset')
        page.get_by_role('button', name='＋ 新建采集任务', exact=True).click()
        editor = page.locator('.dataset-config-panel')
        editor.get_by_label('抓图判断方式', exact=True).select_option('association')
        editor.get_by_label('关联类别', exact=True).fill('helmet')
        editor.get_by_role('button', name='＋ 添加关联类别', exact=True).click()
        editor.get_by_label('关联类别 2', exact=True).fill('bare_head')
        editor.get_by_label('空间匹配方式', exact=True).select_option(relation)
        if custom_values:
            editor.get_by_text('目标匹配的高级设置', exact=True).click()
            editor.get_by_label('主体最低置信度（%）', exact=True).fill('33.5')
            editor.get_by_label('关联类别 1（helmet）最低置信度（%）', exact=True).fill('55.5')
            if relation == 'overlap':
                editor.get_by_label('关联目标至少有多少面积在主体框内（%）', exact=True).fill('50.25')
            elif relation == 'near':
                editor.get_by_label('最大距离（主体框对角线的倍数）', exact=True).fill('1.025')
            editor.get_by_text('目标匹配的高级设置', exact=True).click()
            editor.get_by_label('抓图间隔（秒）', exact=True).fill('2.75')
            editor.get_by_label('运行时长（小时）').fill('0.5')
            editor.get_by_text('高级设置', exact=True).click()
            editor.get_by_label('条件持续时间（秒）', exact=True).fill('0.25')
            editor.get_by_text('高级设置', exact=True).click()
        button = page.get_by_role('button', name='创建任务', exact=True)
        expect(button).to_be_enabled()
        invalid = editor.locator('form').evaluate('''form => [...form.elements]
            .filter(e => e.willValidate && !e.validity.valid)
            .map(e => ({label: e.closest('label')?.textContent, value: e.value,
                        min: e.min, step: e.step, message: e.validationMessage}))''')
        assert not invalid, invalid
        button.click()
        expect(editor).to_have_count(0)
        config = json.loads(manager.rows()[0]['config'])
        saved = config['rules'][0]['condition']['association']
        assert saved['relation'] == relation and len(saved['objects']) == 2
        assert saved['min_overlap'] == (.5025 if custom_values and relation == 'overlap' else .5)
        assert saved['max_distance'] == (1.025 if custom_values and relation == 'near' else 1)
        if custom_values:
            assert saved['subject']['min_confidence'] == .335
            assert saved['objects'][0]['min_confidence'] == .555
            assert config['interval_sec'] == 2.75 and config['duration_hours'] == .5 and config['confirm_sec'] == .25
        logic = page.locator('.dataset-task').get_by_role('region', name='取图逻辑', exact=True)
        expect(logic).to_be_visible()
        expect(logic).to_contain_text('没有匹配到以下任何一类')
        expect(logic).to_contain_text(f'person（置信度 ≥ {33.5 if custom_values else 30}%）')
        expect(logic).to_contain_text(f'helmet（置信度 ≥ {55.5 if custom_values else 30}%）')
        expect(logic).to_contain_text('bare_head（置信度 ≥ 30%）')
        expect(logic).to_contain_text('一对一匹配')
        if relation == 'overlap':
            expect(logic).to_contain_text(f'至少 {50.25 if custom_values else 50}% 的面积在 person 框内')
        elif relation == 'near':
            expect(logic).to_contain_text(f'person 框对角线的 {1.025 if custom_values else 1} 倍')
        else:
            expect(logic).to_contain_text('helmet 或 bare_head 的中心在 person 框内')
        expect(logic).to_contain_text(f'两次抓图至少间隔 {2.75 if custom_values else 2} 秒')
        expect(logic).to_contain_text('每种情况持续满足 0.25 秒' if custom_values else '当帧满足即可')
        browser.close()


@pytest.mark.parametrize('trigger_mode', ['periodic', 'on_enter'])
def test_task_card_shows_complete_nested_logic_and_saved_capture_policy(console, trigger_mode):
    url, manager = console
    person = {'class': 'person', 'min_confidence': .335}
    helmet = {'class': 'helmet', 'min_confidence': .555}
    head = {'class': 'bare_head', 'min_confidence': .65}
    config = dataset.TaskConfig(rules=[
        {'id': 'r1', 'name': '复合条件', 'condition': {'all': [
            {'association': {'subject': person, 'objects': [helmet, head], 'state': 'unmatched',
                             'relation': 'overlap', 'min_overlap': .625, 'quantifier': 'count',
                             'count': {'op': '>=', 'value': 2}, 'one_to_one': False}},
            {'any': [{'compare_counts': {'left': helmet, 'right': head, 'op': '>'}},
                     {'not': dict(person, count={'op': '>=', 'value': 1})}]},
        ]}},
        {'id': 'r2', 'name': '多关联目标', 'condition': {'association': {
            'subject': person, 'objects': [helmet, head], 'state': 'count', 'object_count': {'op': '>', 'value': 1},
            'relation': 'near', 'max_distance': 1.025, 'quantifier': 'all'}}},
        {'id': 'r3', 'name': '反向关联', 'condition': {'not': {'association': {
            'subject': person, 'object': helmet, 'state': 'matched', 'relation': 'center_inside'}}}},
        {'id': 'r4', 'name': '数量判断', 'condition': dict(head, count={'op': '!=', 'value': 2})},
    ], interval_sec=2.75, confirm_sec=.25, max_samples=600, images_per_folder=45,
       duration_hours=.5, roi_name='作业区域', roi_anchor='foot', trigger_mode=trigger_mode).model_dump()
    manager.save('demo', 0, config)
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                     headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context()
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/dataset')
        logic = page.locator('.dataset-task').get_by_role('region', name='取图逻辑', exact=True)
        expect(logic.locator('li')).to_have_count(4)
        expect(logic).to_contain_text('满足以下任意一种情况即可抓图（或者），同一帧只保存一张')
        first = logic.locator('li').nth(0)
        for text in ('至少 2 个 person（置信度 ≥ 33.5%）', 'helmet（置信度 ≥ 55.5%）',
                     'bare_head（置信度 ≥ 65%）', '至少 62.5% 的面积在 person 框内', '允许复用关联目标',
                     '并且', '或者', '的数量大于', '没检测到 person（置信度 ≥ 33.5%）'):
            expect(first).to_contain_text(text)
        counted = logic.locator('li').nth(1)
        for text in ('全部 person', '关联目标总数量超过 1 个', '对角线的 1.025 倍', '逐主体统计'):
            expect(counted).to_contain_text(text)
        expect(counted).not_to_contain_text('一对一匹配')
        expect(logic.locator('li').nth(2)).to_contain_text('以下情况不成立')
        expect(logic.locator('li').nth(2)).to_contain_text('一对一匹配')
        expect(logic.locator('li').nth(3)).to_contain_text('bare_head（置信度 ≥ 65%） 的数量不是 2 个')
        for text in ('采集区域“作业区域”，按目标脚点', '持续满足 0.25 秒', '至少间隔 2.75 秒',
                     '每文件夹 45 张', '累计保存达到 600 张', '经过 0.5 小时', '任一条件即停止'):
            expect(logic).to_contain_text(text)
        expect(logic).to_contain_text('从不满足变为满足时抓一次' if trigger_mode == 'on_enter' else '条件满足时按间隔持续抓图')
        # Long nested conditions remain fully visible on desktop and mobile.
        for width in (1366, 390):
            page.set_viewport_size({'width': width, 'height': 1100})
            size = logic.evaluate('(e) => [e.clientWidth, e.scrollWidth]')
            assert size[1] <= size[0] + 1, (width, size)
            expect(logic.locator('li').nth(3)).to_be_visible()
        page.reload()
        expect(logic.locator('li')).to_have_count(4)
        expect(logic).to_contain_text('至少间隔 2.75 秒')
        assert manager.rows()[0]['config'] == json.dumps(config, ensure_ascii=False, sort_keys=True)
        assert not errors, errors
        browser.close()


@pytest.mark.parametrize('relation', ['subject_center_inside', 'subject_overlap'])
def test_reverse_containment_labels_create_edit_and_task_card(console, relation):
    url, manager = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                     headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context()
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        page.goto(url + '/dataset')
        page.get_by_role('button', name='＋ 新建采集任务', exact=True).click()
        editor = page.locator('.dataset-config-panel')
        editor.get_by_label('抓图判断方式', exact=True).select_option('association')
        editor.get_by_label('主体类别', exact=True).fill('helmet')
        editor.get_by_label('关联类别', exact=True).fill('person')
        matching = editor.get_by_label('空间匹配方式', exact=True)
        expect(matching.locator('option[value="overlap"]')).to_have_text('person 至少 50% 的面积在 helmet 框内')
        expect(matching.locator('option[value="subject_overlap"]')).to_have_text('helmet 至少 50% 的面积在 person 框内')
        expect(matching.locator('option[value="center_inside"]')).to_have_text('person 的中心在 helmet 框内')
        expect(matching.locator('option[value="subject_center_inside"]')).to_have_text('helmet 的中心在 person 框内')
        matching.select_option(relation)
        expect(editor.get_by_text('一对一：每个 person 最多匹配一个 helmet。', exact=True)).to_be_visible()
        editor.get_by_text('目标匹配的高级设置', exact=True).click()
        one_to_one = editor.get_by_label('每个关联目标只分配给一个主体', exact=True)
        expect(one_to_one).to_be_checked()
        one_to_one.uncheck()
        if relation == 'subject_overlap':
            editor.get_by_label('主体至少有多少面积在关联目标框内（%）', exact=True).fill('62.5')
            expect(editor).to_contain_text('计算方式：两框交集面积 ÷ 主体框面积')
        expected = 'helmet 至少 62.5% 的面积在 person 框内' if relation == 'subject_overlap' else 'helmet 的中心在 person 框内'
        expect(editor.locator('.dataset-rule-preview')).to_contain_text(expected)
        page.get_by_role('button', name='创建任务', exact=True).click()
        expect(editor).to_have_count(0)
        saved = json.loads(manager.rows()[0]['config'])['rules'][0]['condition']['association']
        assert saved['subject']['class'] == 'helmet' and saved['object']['class'] == 'person'
        assert saved['relation'] == relation and saved['state'] == 'unmatched' and not saved['one_to_one']
        logic = page.locator('.dataset-task').get_by_role('region', name='取图逻辑', exact=True)
        expect(logic).to_contain_text(expected)
        expect(logic).to_contain_text('允许复用关联目标')
        for width in (1366, 390):
            page.set_viewport_size({'width': width, 'height': 1100})
            size = logic.evaluate('(e) => [e.clientWidth, e.scrollWidth]')
            assert size[1] <= size[0] + 1, (width, size)
        page.reload()
        expect(logic).to_contain_text(expected)
        page.get_by_role('button', name='编辑配置', exact=True).click()
        expect(matching).to_have_value(relation)
        editor.get_by_text('目标匹配的高级设置', exact=True).click()
        expect(editor.get_by_label('每个关联目标只分配给一个主体', exact=True)).not_to_be_checked()
        # Switching back keeps the explicitly chosen direction and old semantics.
        matching.select_option('overlap')
        expect(editor.locator('.dataset-rule-preview')).to_contain_text('person 至少')
        page.get_by_role('button', name='保存修改', exact=True).click()
        expect(editor).to_have_count(0)
        assert json.loads(manager.rows()[0]['config'])['rules'][0]['condition']['association']['relation'] == 'overlap'
        browser.close()


def test_invalid_saved_draft_opens_advanced_field_and_explains_submission_error(console):
    url, manager = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                     headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context()
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        page.goto(url + '/dataset')
        page.get_by_role('button', name='＋ 新建采集任务', exact=True).click()
        editor = page.locator('.dataset-config-panel')
        # 模拟恢复旧草稿的越界值；它不能被提交，也不能藏在折叠面板中毫无反馈。
        page.evaluate('''() => {
            const key = 'rk3588-dataset-drafts', saved = JSON.parse(sessionStorage.getItem(key));
            saved.state.drafts.demo.config.jpeg_quality = 101;
            saved.state.drafts.demo.config.confirm_sec = 61;
            sessionStorage.setItem(key, JSON.stringify(saved));
        }''')
        page.reload()
        expect(editor.get_by_label('JPEG 质量', exact=True)).to_have_count(0)
        confirm = editor.get_by_label('条件持续时间（秒）', exact=True)
        expect(confirm).not_to_be_visible()
        create = page.get_by_role('button', name='创建任务', exact=True)
        expect(create).to_be_enabled()
        create.click()
        expect(confirm).to_be_visible()
        expect(confirm).to_be_focused()
        expect(editor.get_by_role('alert')).to_contain_text('请检查“条件持续时间（秒）”')
        assert manager.rows() == []
        confirm.fill('0.25')
        assert 'jpeg_quality' not in page.evaluate("JSON.parse(sessionStorage.getItem('rk3588-dataset-drafts')).state.drafts.demo.config")
        create.click()
        expect(editor).to_have_count(0)
        config = json.loads(manager.rows()[0]['config'])
        assert 'jpeg_quality' not in config and config['confirm_sec'] == .25
        browser.close()


def test_capture_excess_related_targets_or_missing_targets_and_edit_subject_count(console):
    url, manager = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                     headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context()
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/dataset')
        page.get_by_role('button', name='＋ 新建采集任务', exact=True).click()
        editor = page.locator('.dataset-config-panel')
        first = editor.locator('.dataset-rule').nth(0)
        first.get_by_label('抓图判断方式', exact=True).select_option('association')
        first.get_by_label('关联类别', exact=True).fill('helmet')
        first.get_by_label('关联匹配状态', exact=True).select_option('count')
        expect(first.get_by_label('关联目标数量比较', exact=True)).to_have_value('>')
        expect(first.get_by_label('关联目标个数', exact=True)).to_have_value('1')
        expect(first.locator('.dataset-rule-preview')).to_contain_text('至少一个 person 的 helmet 关联目标数量超过 1 个')
        expect(first.get_by_label('每个关联目标只分配给一个主体', exact=True)).to_have_count(0)
        # 同一任务中的另一条规则仍然用于同主体两类漏检，二者是 OR。
        editor.get_by_role('button', name='＋ 添加另一种抓图情况', exact=True).click()
        second = editor.locator('.dataset-rule').nth(1)
        second.get_by_label('抓图判断方式', exact=True).select_option('association')
        second.get_by_label('主体类别', exact=True).fill('person')
        second.get_by_label('关联类别', exact=True).fill('helmet')
        second.get_by_role('button', name='＋ 添加关联类别', exact=True).click()
        second.get_by_label('关联类别 2', exact=True).fill('bare_head')
        page.reload()
        expect(first.get_by_label('关联匹配状态', exact=True)).to_have_value('count')
        for width in (1366, 390):
            page.set_viewport_size({'width': width, 'height': 1100})
            size = page.locator('.dataset-page').evaluate('(e) => [e.clientWidth, e.scrollWidth]')
            assert size[1] <= size[0] + 1, (width, size)
        page.get_by_role('button', name='创建任务', exact=True).click()
        expect(editor).to_have_count(0)
        rules = json.loads(manager.rows()[0]['config'])['rules']
        counted = rules[0]['condition']['association']
        assert counted['state'] == 'count' and counted['object_count'] == {'op': '>', 'value': 1}
        assert counted['quantifier'] == 'any' and 'one_to_one' not in counted
        assert rules[1]['condition']['association']['state'] == 'unmatched'
        assert len(rules[1]['condition']['association']['objects']) == 2
        page.get_by_role('button', name='编辑配置', exact=True).click()
        first.get_by_label('关联主体数量要求', exact=True).select_option('count')
        first.get_by_label('主体个数', exact=True).fill('2')
        page.get_by_role('button', name='保存修改', exact=True).click()
        expect(editor).to_have_count(0)
        counted = json.loads(manager.rows()[0]['config'])['rules'][0]['condition']['association']
        assert counted['object_count'] == {'op': '>', 'value': 1} and counted['count'] == {'op': '>=', 'value': 2}
        page.get_by_role('button', name='编辑配置', exact=True).click()
        first.get_by_label('关联匹配状态', exact=True).select_option('unmatched')
        expect(first.get_by_label('关联目标数量比较', exact=True)).to_have_count(0)
        page.get_by_role('button', name='保存修改', exact=True).click()
        expect(editor).to_have_count(0)
        original = json.loads(manager.rows()[0]['config'])['rules'][0]['condition']['association']
        assert original['state'] == 'unmatched' and 'object_count' not in original and original['one_to_one']
        assert not errors, errors
        browser.close()


def test_stopped_program_automatically_updates_task_card_and_allows_editing(console, monkeypatch):
    url, manager = console
    running = {'identity': 'vision-process-1'}
    monkeypatch.setattr(manager, 'process_identity', lambda app: running['identity'])
    config = dataset.TaskConfig(rules=[{'id': 'r', 'name': '有人时采集', 'condition': {
        'class': 'person', 'min_confidence': .3, 'count': {'op': '>=', 'value': 1}}}]).model_dump()
    task = manager.save('demo', 0, config)['tasks'][0]
    with manager.db() as db:
        db.execute('UPDATE tasks SET enabled=1,saved=8 WHERE id=?', (task['id'],))
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                     headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context()
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        page.goto(url + '/dataset')
        card = page.locator('.dataset-task')
        expect(card.get_by_role('button', name='暂停采集', exact=True)).to_be_visible()
        expect(card.get_by_role('button', name='查看配置', exact=True)).to_be_visible()
        running['identity'] = None
        # Periodic refresh, without reloading or navigating, updates both actions.
        expect(card.get_by_text('视觉程序已停止，采集任务已自动暂停；重新启动后请点击“开始采集”', exact=True)).to_be_visible(timeout=6000)
        expect(card.get_by_role('button', name='暂停采集', exact=True)).to_have_count(0)
        expect(card.get_by_role('button', name='开始采集', exact=True)).to_be_disabled()
        expect(card).to_contain_text('累计保存 8 张')
        expect(card.get_by_role('region', name='取图逻辑', exact=True)).to_contain_text('视觉程序停止时自动暂停')
        assert not manager.rows()[0]['enabled']
        assert json.loads(manager.rows()[0]['config']) == config
        page.set_viewport_size({'width': 390, 'height': 844})
        size = card.evaluate('(element) => [element.clientWidth, element.scrollWidth]')
        assert size[1] <= size[0] + 1, size
        card.get_by_role('button', name='编辑配置', exact=True).click()
        expect(page.get_by_role('button', name='保存修改', exact=True)).to_be_enabled()
        expect(page.get_by_label('抓图间隔（秒）', exact=True)).to_have_value('2')
        browser.close()


@pytest.fixture
def console(tmp_path, monkeypatch):
    if not (FRONTEND / 'dist/index.html').is_file():
        pytest.skip('Run npm run build in web_console/frontend first')
    apps = []
    for name in ('demo', 'other'):
        assets = tmp_path / name / 'assets'
        assets.mkdir(parents=True)
        (assets / 'labels.txt').write_text('person\nhelmet\nbare_head\n')
        (assets / 'config.json').write_text(json.dumps({'channels': [{
            'id': 0, 'models': [{'model_type': 'yolov8_det', 'label_path': 'assets/labels.txt'}],
        }]}))
        apps.append(dict(name=name, status='stopped', models=[], labels=[], videos=[], config_files=['assets/config.json'],
                         active_config='config.json', uptime=None, has_binary=True, has_config=True))
    manager = DatasetManager(tmp_path)
    monkeypatch.setattr(dataset, 'manager', manager)
    app = FastAPI()
    app.include_router(dataset.router, prefix='/api')

    @app.get('/api/apps')
    def list_apps(): return apps

    @app.api_route('/api/{path:path}', methods=['GET', 'POST'])
    def ancillary_api(path: str): return {}

    app.mount('/assets', StaticFiles(directory=FRONTEND / 'dist/assets'), name='assets')

    @app.get('/{path:path}')
    def page(path: str): return FileResponse(FRONTEND / 'dist/index.html')

    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    port = listener.getsockname()[1]
    server = uvicorn.Server(uvicorn.Config(app, log_level='error', lifespan='off'))
    worker = threading.Thread(target=lambda: server.run(sockets=[listener]), daemon=True)
    worker.start()
    for _ in range(100):
        if server.started: break
        time.sleep(.02)
    assert server.started
    try:
        yield f'http://127.0.0.1:{port}', manager
    finally:
        server.should_exit = True
        worker.join(timeout=5)
        listener.close()


def test_offline_create_and_drafts_survive_navigation_refresh_and_app_switch(console):
    url, manager = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                     headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context()
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/dataset')
        create = page.get_by_role('button', name='＋ 新建采集任务', exact=True)
        editor = page.locator('.dataset-config-panel')
        expect(create).to_be_enabled()
        create.click()
        interval = page.get_by_label('抓图间隔（秒）', exact=True)
        interval.fill('7')
        page.get_by_label('每个文件夹图片张数', exact=True).fill('45')
        page.get_by_label('条件 1 类别', exact=True).fill('helmet')
        page.get_by_label('接收程序类型', exact=True).select_option('python')
        # Navigation must not ask to discard a draft; it remains editable.
        page.locator('.sidebar a[href="/"]').click()
        page.locator('.sidebar a[href="/dataset"]').click()
        expect(interval).to_have_value('7')
        expect(page.get_by_label('每个文件夹图片张数', exact=True)).to_have_value('45')
        expect(page.get_by_label('条件 1 类别', exact=True)).to_have_value('helmet')
        page.reload()
        expect(interval).to_have_value('7')
        expect(page.get_by_label('接收程序类型', exact=True)).to_have_value('python')
        # Each app has its own draft; restore the selected app on re-entry.
        page.get_by_label('采集程序', exact=True).select_option('other')
        expect(editor).to_have_count(0)
        expect(create).to_be_enabled()
        create.click()
        interval.fill('11')
        page.locator('.sidebar a[href="/"]').click()
        page.locator('.sidebar a[href="/dataset"]').click()
        expect(page.get_by_label('采集程序', exact=True)).to_have_value('other')
        expect(interval).to_have_value('11')
        page.get_by_label('采集程序', exact=True).select_option('demo')
        expect(interval).to_have_value('7')
        page.get_by_role('button', name='创建任务', exact=True).click()
        expect(editor).to_have_count(0)
        task = page.locator('.dataset-task')
        expect(task).to_have_count(1)
        logic = task.get_by_role('region', name='取图逻辑', exact=True)
        expect(logic).to_contain_text('检测到 helmet（置信度 ≥ 30%）')
        expect(logic).to_contain_text('不限张数和时长，手动暂停')
        expect(logic).to_contain_text('整个推理范围')
        expect(task.get_by_role('button', name='开始采集', exact=True)).to_be_disabled()
        assert manager.rows()[0]['enabled'] == 0
        saved = json.loads(manager.rows()[0]['config'])
        assert saved['interval_sec'] == 7 and saved['images_per_folder'] == 45
        assert saved['rules'][0]['condition']['all'][0]['class'] == 'helmet'
        # Offline editing also works, and failed saves preserve the form.
        task.get_by_role('button', name='编辑配置', exact=True).click()
        interval.fill('9')
        expect(logic).to_contain_text('至少间隔 7 秒')  # The card describes saved settings.
        page.get_by_label('条件 1 类别', exact=True).fill('unknown')
        page.get_by_role('button', name='保存修改', exact=True).click()
        expect(editor.get_by_role('alert')).to_contain_text('模型标签一致')
        page.locator('.sidebar a[href="/"]').click()
        page.locator('.sidebar a[href="/dataset"]').click()
        expect(interval).to_have_value('9')
        expect(page.get_by_label('条件 1 类别', exact=True)).to_have_value('unknown')
        page.get_by_label('条件 1 类别', exact=True).fill('person')
        page.get_by_role('button', name='保存修改', exact=True).click()
        expect(editor).to_have_count(0)
        assert json.loads(manager.rows()[0]['config'])['interval_sec'] == 9
        expect(logic).to_contain_text('至少间隔 9 秒')
        expect(logic).to_contain_text('检测到 person（置信度 ≥ 30%）')
        page.reload()
        expect(editor).to_have_count(0)
        expect(task).to_contain_text('间隔 9 秒')
        # Cancel intentionally discards a draft; cancel confirmation can keep it.
        page.get_by_label('采集程序', exact=True).select_option('other')
        expect(interval).to_have_value('11')
        page.once('dialog', lambda dialog: dialog.dismiss())
        page.get_by_role('button', name='取消', exact=True).click()
        expect(editor).to_be_visible()
        page.once('dialog', lambda dialog: dialog.accept())
        page.get_by_role('button', name='取消', exact=True).click()
        expect(editor).to_have_count(0)
        page.reload()
        expect(editor).to_have_count(0)
        # Logout clears browser drafts without removing saved tasks from device.
        create.click()
        interval.fill('12')
        page.get_by_role('button', name='退出登录', exact=True).click()
        expect(page).to_have_url(url + '/login')
        state = page.evaluate("JSON.parse(sessionStorage.getItem('rk3588-dataset-drafts')).state")
        assert state['drafts'] == {} and state['app'] == ''
        assert len(manager.rows()) == 1
        assert not errors, errors
        browser.close()


def test_multiple_related_categories_edit_refresh_and_legacy_compatibility(console):
    url, manager = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                     headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context()
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/dataset')
        page.get_by_role('button', name='＋ 新建采集任务', exact=True).click()
        editor = page.locator('.dataset-config-panel')
        editor.get_by_label('抓图判断方式', exact=True).select_option('association')
        editor.get_by_label('关联类别', exact=True).fill('helmet')
        # 单类别先保存为旧格式，已有任务仍能增加、删除并分别设置阈值。
        page.get_by_role('button', name='创建任务', exact=True).click()
        expect(editor).to_have_count(0)
        legacy = json.loads(manager.rows()[0]['config'])['rules'][0]['condition']['association']
        assert legacy['object']['class'] == 'helmet' and 'objects' not in legacy
        page.get_by_role('button', name='编辑配置', exact=True).click()
        editor.get_by_role('button', name='＋ 添加关联类别', exact=True).click()
        second = editor.get_by_label('关联类别 2', exact=True)
        second.fill('helmet')
        expect(editor.get_by_text('关联类别不能重复，请修改或删除重复项。', exact=True)).to_be_visible()
        second.fill('bare_head')
        expect(editor.get_by_label('关联匹配状态', exact=True).locator('option:checked')).to_have_text('以下类别都没匹配到')
        expect(editor.locator('.dataset-rule-preview')).to_contain_text('至少一个 person 没有匹配到以下任何一类：helmet、bare_head')
        editor.get_by_text('目标匹配的高级设置', exact=True).click()
        editor.get_by_label('关联类别 2（bare_head）最低置信度（%）', exact=True).fill('65')
        page.locator('.sidebar a[href="/"]').click()
        page.locator('.sidebar a[href="/dataset"]').click()
        expect(second).to_have_value('bare_head')
        page.reload()
        expect(second).to_have_value('bare_head')
        editor.get_by_text('目标匹配的高级设置', exact=True).click()
        expect(editor.get_by_label('关联类别 2（bare_head）最低置信度（%）', exact=True)).to_have_value('65')
        for width in (1366, 390):
            page.set_viewport_size({'width': width, 'height': 1100})
            size = page.locator('.dataset-page').evaluate('(e) => [e.clientWidth, e.scrollWidth]')
            assert size[1] <= size[0] + 1, (width, size)
        page.get_by_role('button', name='保存修改', exact=True).click()
        expect(editor).to_have_count(0)
        saved = json.loads(manager.rows()[0]['config'])['rules'][0]['condition']['association']
        assert saved['objects'] == [{'class': 'helmet', 'min_confidence': .3},
                                    {'class': 'bare_head', 'min_confidence': .65}]
        assert 'object' not in saved and saved['state'] == 'unmatched' and saved['quantifier'] == 'any'
        page.get_by_role('button', name='编辑配置', exact=True).click()
        editor.get_by_label('关联匹配状态', exact=True).select_option('matched')
        expect(editor.locator('.dataset-rule-preview')).to_contain_text('匹配到以下至少一类：helmet、bare_head')
        editor.get_by_role('button', name='删除关联类别 1', exact=True).click()
        expect(editor.get_by_label('关联类别', exact=True)).to_have_value('bare_head')
        page.get_by_role('button', name='保存修改', exact=True).click()
        expect(editor).to_have_count(0)
        single = json.loads(manager.rows()[0]['config'])['rules'][0]['condition']['association']
        assert single['object'] == {'class': 'bare_head', 'min_confidence': .65} and 'objects' not in single
        assert not errors, errors
        browser.close()


def test_plain_language_controls_preserve_actual_rule_logic(console):
    url, manager = console
    expect = playwright.expect
    with playwright.sync_playwright() as pw:
        browser = pw.chromium.launch(executable_path=os.environ.get('CHROMIUM_PATH', '/usr/bin/chromium'),
                                     headless=True, args=['--no-sandbox', '--disable-dev-shm-usage'])
        context = browser.new_context()
        context.add_init_script("localStorage.setItem('rk3588-auth', JSON.stringify({state:{token:'test',username:'tester'},version:0}))")
        page = context.new_page()
        errors = []
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.goto(url + '/dataset')
        page.get_by_role('button', name='＋ 新建采集任务', exact=True).click()
        editor = page.locator('.dataset-config-panel')
        expect(editor.get_by_label('条件组名称', exact=True)).to_have_count(0)
        editor.get_by_text('更多条件设置', exact=True).click()
        editor.get_by_text('添加条件，或在条件不满足时抓图', exact=True).click()
        expect(editor.get_by_text('改后抓图条件：没检测到 person', exact=True)).to_be_visible()
        editor.get_by_role('button', name='改成不满足当前条件时抓图', exact=True).click()
        expect(editor.locator('.dataset-rule-preview')).to_contain_text('没检测到 person → 保存图片')
        page.get_by_role('button', name='创建任务', exact=True).click()
        expect(editor).to_have_count(0)
        saved = json.loads(manager.rows()[0]['config'])['rules'][0]
        assert saved['condition']['not']['class'] == 'person'
        page.get_by_role('button', name='编辑配置', exact=True).click()
        editor.get_by_text('更多条件设置', exact=True).click()
        editor.get_by_role('button', name='恢复为满足下面条件时抓图', exact=True).click()
        expect(editor.locator('.dataset-rule-preview')).to_contain_text('检测到 person → 保存图片')
        editor.get_by_label('抓图判断方式', exact=True).select_option('association')
        editor.get_by_label('关联类别', exact=True).fill('helmet')
        expect(editor.get_by_text('更多条件设置', exact=True)).to_have_count(0)
        editor.get_by_text('添加条件，或在条件不满足时抓图', exact=True).click()
        editor.get_by_role('button', name='再加一条必须满足的条件', exact=True).click()
        editor.get_by_text('更多条件设置', exact=True).click()
        editor.get_by_label('类别名称', exact=True).fill('person')
        editor.get_by_label('检测条件', exact=True).select_option('count')
        editor.get_by_label('目标个数', exact=True).fill('2')
        # Expanded condition controls fit both desktop and mobile widths.
        for width in (1366, 390):
            page.set_viewport_size({'width': width, 'height': 1100})
            size = page.locator('.dataset-page').evaluate('(e) => [e.clientWidth, e.scrollWidth]')
            assert size[1] <= size[0] + 1, (width, size)
        page.get_by_role('button', name='保存修改', exact=True).click()
        expect(editor).to_have_count(0)
        updated = json.loads(manager.rows()[0]['config'])['rules'][0]
        assert updated['name'] == saved['name']
        conditions = updated['condition']['all']
        assert conditions[0]['association']['subject']['class'] == 'person'
        assert conditions[0]['association']['object']['class'] == 'helmet'
        assert conditions[1]['count'] == {'op': '>=', 'value': 2}
        assert not errors, errors
        browser.close()
