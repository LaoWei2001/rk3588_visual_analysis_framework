"""Offline task persistence and validation do not require an engine binary."""
import copy
import json
import shutil

import pytest
from fastapi import HTTPException
from routers.dataset import TaskConfig
from services.dataset_manager import DatasetManager


@pytest.fixture
def offline(tmp_path):
    assets = tmp_path / 'demo' / 'assets'
    assets.mkdir(parents=True)
    (assets / 'labels.txt').write_text('person\nhelmet\n')
    config = {'channels': [{'id': 3, 'models': [{'model_type': 'yolov8_det',
               'label_path': 'assets/labels.txt', 'obj_thresh': .4, 'detect_classes': ['person', 'helmet']}]}]}
    (assets / 'config.json').write_text(json.dumps(config))
    return DatasetManager(tmp_path), assets, config


def task_config(condition=None):
    return TaskConfig(rules=[{'id': 'r1', 'name': '采集条件', 'condition': condition or {
        'class': 'person', 'min_confidence': .5, 'count': {'op': '>=', 'value': 1}}}]).model_dump()


@pytest.mark.parametrize('legacy_quality', [60, 95, 100, 101, None])
def test_legacy_image_quality_is_ignored_and_not_saved(offline, legacy_quality):
    manager, _, _ = offline
    config = task_config()
    assert 'jpeg_quality' not in config
    legacy = dict(config, jpeg_quality=legacy_quality)
    assert TaskConfig(**legacy).model_dump() == config
    assert manager.save('demo', 3, legacy)['tasks'][0]['config'] == config
    assert json.loads(manager.rows()[0]['config']) == config
    assert legacy['jpeg_quality'] == legacy_quality  # No mutation of callers.


def test_existing_quality_setting_is_hidden_without_changing_task_identity(offline):
    manager, _, _ = offline
    task = manager.save('demo', 3, task_config())['tasks'][0]
    legacy_config = json.dumps(dict(task['config'], jpeg_quality=60))
    with manager.db() as db:
        db.execute('UPDATE tasks SET config=?, enabled=1, saved=7 WHERE id=?', (legacy_config, task['id']))
    before = manager.rows()[0]
    proof = manager.proof(task['id'], before['revision'], 'pending-image')
    restored = DatasetManager(manager.root, process_identity=lambda app: 'still-running')
    listed = restored.list('demo')['tasks'][0]
    assert listed['config'] == task['config'] and listed['id'] == task['id']
    assert listed['enabled'] and listed['saved'] == 7
    assert 'jpeg_quality' not in restored.command(before)
    assert restored.rows()[0] == before
    assert restored.proof(task['id'], before['revision'], 'pending-image') == proof


def test_create_edit_restore_and_delete_without_engine(offline):
    manager, assets, _ = offline
    status = manager.save('demo', 3, task_config())
    task = status['tasks'][0]
    assert status['engine_error'] and '等待视觉程序启动' in task['status']
    assert not task['enabled'] and task['deadline'] == 0
    updated = task_config()
    updated.update(interval_sec=7, duration_hours=2, max_samples=100)
    edited = manager.save('demo', 3, updated)['tasks'][0]
    assert edited['id'] == task['id'] and edited['config'] == updated
    restored = DatasetManager(manager.root)
    assert restored.list('demo')['tasks'][0]['config'] == updated
    assert restored.rows()[0]['deadline'] == 0
    # Creating a task does not start the engine or start its duration clock.
    with pytest.raises(HTTPException) as rejected:
        restored.set_enabled('demo', task['id'], True)
    assert rejected.value.status_code == 503
    assert not restored.rows()[0]['enabled'] and restored.rows()[0]['deadline'] == 0
    assert restored.remove('demo', task['id'])['tasks'] == []


def test_pause_is_scoped_to_app_and_preserves_offline_drafts(offline):
    manager, _, _ = offline
    first = manager.save('demo', 3, task_config())['tasks'][0]
    with manager.db() as db:
        db.execute('UPDATE tasks SET enabled=1 WHERE id=?', (first['id'],))
        db.execute('INSERT INTO tasks VALUES (?,?,?,?,?,?,?,?)',
                   ('another-task', 'other', 3, json.dumps(first['config']), first['id'], 1, 9, 123))
    manager.pause_app('demo')
    assert manager.rows('demo')[0]['enabled'] == 0
    assert manager.rows('other')[0]['enabled'] == 1
    assert manager.rows('other')[0]['saved'] == 9
    manager.remove('demo', first['id'])
    draft = manager.save('demo', 3, task_config())['tasks'][0]
    manager.pause_app('demo')
    assert '等待视觉程序启动' in manager.list('demo')['tasks'][0]['status']
    with manager.db() as db:
        assert not db.execute('SELECT 1 FROM task_runtime WHERE task=?', (first['id'],)).fetchone()
        assert not db.execute('SELECT 1 FROM task_runtime WHERE task=?', (draft['id'],)).fetchone()


def test_deleted_app_records_are_automatically_paused_without_breaking_maintenance(offline):
    manager, assets, _ = offline
    task = manager.save('demo', 3, task_config())['tasks'][0]
    with manager.db() as db:
        db.execute('UPDATE tasks SET enabled=1 WHERE id=?', (task['id'],))
    before = manager.rows()[0]
    shutil.rmtree(assets.parent)
    _, error = manager.sync('demo')
    assert error == '程序不存在'
    assert manager.rows()[0] == dict(before, enabled=0)
    with manager.db() as db:
        assert '自动暂停' in db.execute('SELECT pause_reason FROM task_runtime WHERE task=?', (task['id'],)).fetchone()[0]


@pytest.mark.parametrize('condition', [
    {'class': 'unknown', 'count': {'op': '>=', 'value': 1}},
    {'class': 'person', 'min_confidence': .3, 'count': {'op': '>=', 'value': 1}},
    {'class': 'person', 'min_confidence': True, 'count': {'op': '>=', 'value': 1}},
    {'class': 'person', 'min_confidence': float('inf'), 'count': {'op': '>=', 'value': 1}},
    {'class': 'person', 'min_confidence': .5, 'count': {'op': 'bad', 'value': 1}},
    {'class': 'person', 'min_confidence': .5, 'count': {'op': '>=', 'value': 1.5}},
    {'class': 'person', 'min_confidence': .5, 'count': {'op': '>=', 'value': True}},
    {'all': []}, {'any': 'bad'}, {'not': None},
    {'all': [], 'class': 'person'},
    {'compare_counts': {'left': None, 'right': None, 'op': '>'}},
    {'association': {'subject': {}, 'object': {}, 'relation': 'bad', 'state': 'matched'}},
    {'class': 'person', 'min_confidence': .5, 'extra': 1, 'count': {'op': '>=', 'value': 1}},
])
def test_invalid_offline_conditions_are_not_saved(offline, condition):
    manager, _, _ = offline
    # Invalid numbers also report a validation error instead of an HTTP 500.
    with pytest.raises(HTTPException) as rejected:
        manager.save('demo', 3, task_config(condition))
    assert rejected.value.status_code == 409 and manager.rows() == []


@pytest.mark.parametrize('change', ['disabled', 'multi_model', 'pose', 'filtered', 'missing_labels', 'duplicate_labels'])
def test_invalid_offline_channel_is_rejected(offline, change):
    manager, assets, app_config = offline
    channel = app_config['channels'][0]
    model = channel['models'][0]
    if change == 'disabled': channel['enable'] = False
    elif change == 'multi_model': channel['models'].append(copy.deepcopy(model))
    elif change == 'pose': model['model_type'] = 'yolov8_pose'
    elif change == 'filtered': model['detect_classes'] = ['helmet']
    elif change == 'missing_labels': (assets / 'labels.txt').unlink()
    else: (assets / 'labels.txt').write_text('person\nperson\n')
    (assets / 'config.json').write_text(json.dumps(app_config))
    with pytest.raises(HTTPException) as rejected:
        manager.save('demo', 3, task_config())
    assert rejected.value.status_code == 409 and manager.rows() == []


def test_offline_nested_comparison_and_association_rules(offline):
    manager, _, _ = offline
    person = {'class': 'person', 'min_confidence': .5}
    helmet = {'class': 'helmet', 'min_confidence': .5}
    condition = {'all': [
        {'compare_counts': {'left': person, 'right': helmet, 'op': '>'}},
        {'not': {'association': {'subject': person, 'object': helmet, 'relation': 'center_inside',
                                'state': 'matched', 'quantifier': 'count', 'count': {'op': '>=', 'value': 1},
                                'one_to_one': True, 'min_overlap': .5, 'max_distance': 1}}},
    ]}
    assert manager.save('demo', 3, task_config(condition))['tasks'][0]['config']['rules'][0]['condition'] == condition
    before = manager.rows()[0]['config']
    condition['all'][1]['not']['association']['quantifier'] = 'bad'
    with pytest.raises(HTTPException): manager.save('demo', 3, task_config(condition))
    assert manager.rows()[0]['config'] == before


def test_offline_rule_ids_and_complexity_limits(offline):
    manager, _, _ = offline
    config = task_config()
    config['rules'] *= 2
    with pytest.raises(HTTPException): manager.save('demo', 3, config)
    config = task_config()
    for _ in range(9): config['rules'][0]['condition'] = {'not': config['rules'][0]['condition']}
    with pytest.raises(HTTPException): manager.save('demo', 3, config)
    config = task_config()
    config['rules'][0]['condition'] = {'all': [config['rules'][0]['condition']] * 256}
    with pytest.raises(HTTPException): manager.save('demo', 3, config)
    assert manager.rows() == []


def test_offline_uses_selected_run_config(offline):
    manager, assets, app_config = offline
    (assets / 'config.json').write_text('{"channels":[]}')
    (assets / 'custom.json').write_text(json.dumps(app_config))
    (assets.parent / 'run.config').write_text('custom.json')
    assert manager.save('demo', 3, task_config())['tasks'][0]['channel_id'] == 3


def multi_association(objects):
    return {'association': {'subject': {'class': 'person', 'min_confidence': .5},
                           'objects': objects, 'relation': 'center_inside', 'state': 'unmatched'}}


@pytest.mark.parametrize('relation', ['subject_center_inside', 'subject_overlap'])
@pytest.mark.parametrize('count_mode', [False, True])
def test_offline_reverse_containment_can_be_created_and_restored(offline, relation, count_mode):
    manager, _, _ = offline
    value = {'subject': {'class': 'helmet', 'min_confidence': .5},
             'object': {'class': 'person', 'min_confidence': .5},
             'relation': relation, 'min_overlap': .625, 'state': 'unmatched', 'one_to_one': False}
    if count_mode:
        value.update(state='count', object_count={'op': '==', 'value': 0})
        del value['one_to_one']
    config = task_config({'association': value})
    assert manager.save('demo', 3, config)['tasks'][0]['config'] == config
    assert DatasetManager(manager.root).list('demo')['tasks'][0]['config'] == config


def test_offline_per_subject_related_count_roundtrip(offline):
    manager, _, _ = offline
    condition = {'association': {'subject': {'class': 'person', 'min_confidence': .5},
        'object': {'class': 'helmet', 'min_confidence': .5}, 'relation': 'center_inside', 'state': 'count',
        'object_count': {'op': '>', 'value': 1}, 'quantifier': 'count', 'count': {'op': '>=', 'value': 2}}}
    saved = manager.save('demo', 3, task_config(condition))['tasks'][0]
    assert saved['config']['rules'][0]['condition'] == condition
    assert DatasetManager(manager.root).list('demo')['tasks'][0]['config'] == saved['config']


@pytest.mark.parametrize('change', ['missing', 'negative', 'fraction', 'boolean', 'bad_op', 'one_true', 'one_false', 'wrong_state'])
def test_invalid_related_count_is_rejected(offline, change):
    manager, _, _ = offline
    value = {'subject': {'class': 'person', 'min_confidence': .5},
             'object': {'class': 'helmet', 'min_confidence': .5}, 'relation': 'center_inside', 'state': 'count',
             'object_count': {'op': '>', 'value': 1}}
    if change == 'missing': del value['object_count']
    elif change == 'negative': value['object_count']['value'] = -1
    elif change == 'fraction': value['object_count']['value'] = 1.5
    elif change == 'boolean': value['object_count']['value'] = True
    elif change == 'bad_op': value['object_count']['op'] = 'bad'
    elif change.startswith('one_'): value['one_to_one'] = change == 'one_true'
    else: value['state'] = 'matched'
    with pytest.raises(HTTPException) as error:
        manager.save('demo', 3, task_config({'association': value}))
    assert error.value.status_code == 409 and manager.rows() == []


def test_offline_multiple_related_categories_persist_and_sync_validation(offline):
    manager, assets, app_config = offline
    (assets / 'labels.txt').write_text('person\nhelmet\nbare_head\n')
    app_config['channels'][0]['models'][0]['detect_classes'].append('bare_head')
    (assets / 'config.json').write_text(json.dumps(app_config))
    condition = multi_association([{'class': 'helmet', 'min_confidence': .5},
                                   {'class': 'bare_head', 'min_confidence': .7}])
    saved = manager.save('demo', 3, task_config(condition))['tasks'][0]
    assert not saved['enabled'] and saved['config']['rules'][0]['condition'] == condition
    assert DatasetManager(manager.root).list('demo')['tasks'][0]['config'] == saved['config']
    # 各类别独立校验模型过滤和输出阈值，失败不得覆盖已保存的多类别配置。
    before = manager.rows()[0]['config']
    condition['association']['objects'][1]['min_confidence'] = .3
    with pytest.raises(HTTPException) as error:
        manager.save('demo', 3, task_config(condition))
    assert error.value.status_code == 409
    condition['association']['objects'][1]['min_confidence'] = .7
    app_config['channels'][0]['models'][0]['detect_classes'].remove('bare_head')
    (assets / 'config.json').write_text(json.dumps(app_config))
    with pytest.raises(HTTPException) as error:
        manager.save('demo', 3, task_config(condition))
    assert error.value.status_code == 409 and manager.rows()[0]['config'] == before


@pytest.mark.parametrize('objects', [
    [], None, {}, 'helmet', [None], [{'class': 'unknown', 'min_confidence': .5}],
    [{'class': 'helmet', 'min_confidence': .5}, {'class': 'helmet', 'min_confidence': .6}],
    [{'class': 'helmet', 'min_confidence': .5}] * 33,
    [{'class': 'helmet', 'min_confidence': True}],
    [{'class': 'helmet', 'min_confidence': .3}],
    [{'class': 'helmet', 'min_confidence': .5, 'extra': True}],
])
def test_invalid_related_category_list_is_rejected(offline, objects):
    manager, _, _ = offline
    with pytest.raises(HTTPException) as error:
        manager.save('demo', 3, task_config(multi_association(objects)))
    assert error.value.status_code == 409 and manager.rows() == []


@pytest.mark.parametrize('ambiguous', [False, True])
def test_association_requires_exactly_one_related_target_field(offline, ambiguous):
    manager, _, _ = offline
    condition = multi_association([{'class': 'helmet', 'min_confidence': .5}])
    if ambiguous:
        condition['association']['object'] = {'class': 'helmet', 'min_confidence': .5}
    else:
        del condition['association']['objects']
    with pytest.raises(HTTPException) as error:
        manager.save('demo', 3, task_config(condition))
    assert error.value.status_code == 409 and manager.rows() == []
