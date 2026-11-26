"""Keep Web saves aligned with native startup fields, including disabled nodes."""
import asyncio
import copy
import json
import re
from pathlib import Path

import pytest
from fastapi import HTTPException

from routers import config_io
from services import config_fields as fields

ROOT = Path(__file__).resolve().parents[3]


def test_registered_fields_match_native_loader():
    registration = (ROOT / 'engine/src/application/config/config_init.cpp').read_text()
    assert fields.GLOBAL == set(re.findall(r'REG_G\("([^"]+)"', registration)) | {'global_logics'}
    assert fields.CHANNEL == set(re.findall(r'REG_C\("([^"]+)"', registration)) | {
        'stream', 'models', 'roi_zones', 'inference_roi'}
    loader = (ROOT / 'engine/src/application/config/config.cpp').read_text()
    native_lists = [set(re.findall(r'"([a-z_]+)"', block))
                    for block in re.findall(r'validate_object_keys\([^;]*?\{([^}]+)\}', loader)]
    for expected in (fields.STREAM, fields.MODEL, fields.GLOBAL_LOGIC, fields.ROI,
                     fields.ZONE, fields.POLICY, fields.DELIVERY):
        assert expected in native_lists


def config():
    return {'global': {'global_logics': [{'instance_id': 'summary', 'logic': 'global_default',
             'report_policy': {'enabled': False, 'deliveries': []}}]},
            'channels': [{'id': 0, 'enable': False, 'stream': {'src_type': 'usb', 'device': '/dev/video0'},
              'models': [{'id': 'disabled', 'enable': False}],
              'inference_roi': {'mode': 'roi_only', 'polygon': [[0, 0], [1, 0], [1, 1]]},
              'roi_zones': [{'name': 'area', 'polygon': []}],
              'report_policy': {'enabled': False, 'deliveries': [{'id': 'd', 'enabled': False}]},
              'logic_parameters': {'custom_business_key': 1}}]}


@pytest.mark.parametrize('path,key', [
    (('global',), 'rtsp_codec'), (('global',), 'rtsp_encoder'), (('global',), 'enable_pause_key'),
    ((), 'model_path'), (('global',), 'obj_thresh'), (('channels', 0), 'model_path'),
    (('channels', 0), 'swap_rb'), (('channels', 0), 'report_parameters'),
    (('global', 'global_logics', 0), 'report_parameters'),
    (('channels', 0), 'infer_enable'), (('channels', 0), 'roi_polygon'), (('channels', 0), 'event_video_enable'),
    (('channels', 0, 'stream'), 'fps'), (('channels', 0, 'models', 0), 'threads'),
    (('channels', 0, 'inference_roi'), 'x'), (('channels', 0, 'roi_zones', 0), 'color'),
    (('channels', 0, 'report_policy'), 'parameters'),
    (('channels', 0, 'report_policy', 'deliveries', 0), 'mapping'),
    (('global', 'global_logics', 0), 'old_option'),
    (('global', 'global_logics', 0, 'report_policy'), 'parameters'),
])
@pytest.mark.parametrize('named', [False, True])
def test_unknown_fields_cannot_replace_saved_config(tmp_path, monkeypatch, path, key, named):
    assets = tmp_path / 'demo/assets'
    assets.mkdir(parents=True)
    monkeypatch.setattr(config_io, 'APPS_ROOT', tmp_path)
    monkeypatch.setattr(config_io, 'validate_model_config', lambda *_: pytest.fail('must reject before NPU inspection'))
    target = assets / ('custom.json' if named else 'config.json')
    original = json.dumps(config())
    target.write_text(original)
    value = config()
    parent = value
    for part in path:
        parent = parent[part]
    parent[key] = []
    snapshot = copy.deepcopy(value)
    with pytest.raises(HTTPException) as error:
        asyncio.run(config_io.save_config_file('demo', target.name, value) if named
                    else config_io.save_config('demo', value))
    assert error.value.status_code == 422
    assert key in error.value.detail
    assert target.read_text() == original
    assert value == snapshot


def test_current_configuration_preserves_dynamic_parameters():
    value = config()
    original = copy.deepcopy(value)
    fields.validate_config_fields(value)
    assert value == original
    for path in (ROOT / 'projects/assets').glob('*.json'):
        fields.validate_config_fields(json.loads(path.read_text()))


def test_every_native_global_option_has_a_canvas_control():
    panel = (ROOT / 'web_console/frontend/src/components/GlobalSettingsPanel.tsx').read_text()
    editable = set(re.findall(r"set\('([^']+)'", panel))
    assert fields.GLOBAL == editable | {'global_logics'}


def test_every_channel_and_model_option_has_a_control_or_canvas_structure():
    panel = (ROOT / 'web_console/frontend/src/components/NodeConfigPanel.tsx').read_text()
    stream_form = panel.split('function StreamForm(', 1)[1].split('function ModelForm(', 1)[0]
    stream_controls = set(re.findall(r"set\('([^']+)'", stream_form))
    channel_controls = stream_controls & fields.CHANNEL
    # These options are represented by linked nodes and their dedicated editors.
    assert fields.CHANNEL == channel_controls | {
        'id', 'logic', 'logic_parameters', 'stream', 'models', 'roi_zones',
        'report_policy',
    }
    model_form = panel.split('function ModelForm(', 1)[1].split('function LogicForm(', 1)[0]
    assert fields.MODEL == set(re.findall(r"set\('([^']+)'", model_form)) | {'model_type'}


def test_catalog_cannot_hide_runtime_parameters():
    import importlib.util
    spec = importlib.util.spec_from_file_location('config_audit_catalog', ROOT / 'tools/build/generate_logics_catalog.py')
    catalog = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(catalog)
    schema = {'type': 'object', 'additionalProperties': False, 'properties': {
        'enabled': {'type': 'boolean', 'default': True, 'x-ui-hidden': True},
    }}
    with pytest.raises(catalog.ManifestError, match='runtime parameters must be visible'):
        catalog.validate_parameter_schema(Path('logic.json'), schema)
    result = catalog.build_catalog(ROOT / 'projects')
    for entry in result['channel_logics'] + result['global_logics']:
        assert set(entry['parameters']['properties']) == {param['key'] for param in entry['params']}


def test_stream_report_and_global_logic_fields_have_editor_ownership():
    panel = (ROOT / 'web_console/frontend/src/components/NodeConfigPanel.tsx').read_text()
    stream_form = panel.split('function StreamForm(', 1)[1].split('function ModelForm(', 1)[0]
    setters = set(re.findall(r"set\('([^']+)'", stream_form))
    assert fields.STREAM == (setters & fields.STREAM) | {'src_type'}
    report_form = (ROOT / 'web_console/frontend/src/components/ReportForm.tsx').read_text()
    policy_controls = set(re.findall(r'setPolicy\(\{\s*([a-z_]+):', report_form))
    assert fields.POLICY == policy_controls | {'enabled', 'deliveries'}
    global_form = panel.split('function GlobalLogicForm(', 1)[1].split('function F(', 1)[0]
    global_controls = set(re.findall(r'onUpdate\(node.id, \{\s*([a-z_]+):', global_form))
    assert fields.GLOBAL_LOGIC == global_controls | {
        'channels', 'logic_parameters', 'report_policy', 'media_source_channel_id',
    }
