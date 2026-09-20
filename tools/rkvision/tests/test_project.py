import json
from pathlib import Path
import shutil
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[3]
from tools.rkvision import project_workflow as vision


def test_external_project_lifecycle_and_version_rejection(tmp_path):
    project = tmp_path / 'external-app'
    vision.create(project)
    assert not (project / 'vision_analysis').exists()
    subprocess.run(
        [str(ROOT / 'rkvision'), 'logic', 'add', 'global', 'global_test', str(project)],
        check=True,
    )
    catalog = vision.build_catalog(project / 'logic')
    assert [item['name'] for item in catalog['channel_logics']] == ['logic_example']
    assert [item['name'] for item in catalog['global_logics']] == ['global_test']
    data = json.loads((project / 'project.json').read_text())
    data['engine']['version'] = '0.0.0'
    (project / 'project.json').write_text(json.dumps(data))
    with pytest.raises(ValueError, match='引擎版本不匹配'):
        vision.check(project)


def test_sdk_headers_compile_without_engine_private_includes(tmp_path):
    flags = subprocess.check_output(['pkg-config', '--cflags', 'opencv4'], text=True).split()
    for header in sorted((ROOT / 'vision_analysis/include/rkvision').glob('*.h')):
        source = tmp_path / (header.stem + '.cpp')
        source.write_text(f'#include <rkvision/{header.name}>\nint main() {{ return 0; }}\n')
        subprocess.run(['c++', '-std=c++14', '-fsyntax-only', '-I'+str(ROOT / 'vision_analysis/include'), *flags, str(source)], check=True)


def test_only_project_modules_enter_catalog(tmp_path):
    project = tmp_path / 'isolated-app'
    vision.create(project)
    module = project / 'logic/modules/logic_example'
    shutil.rmtree(module)
    assert vision.build_catalog(project / 'logic')['channel_logics'] == []
    assert not (project / 'logic/catalog.json').exists()


def test_created_project_uses_unified_rkvision_launcher(tmp_path):
    project = tmp_path / 'local-app'
    vision.create(project)
    assert (project / 'CMakeLists.txt').is_file()
    assert (project / 'engine.local.cmake').is_file()
    assert not (project / 'build.sh').exists()
    assert not (project / 'build.py').exists()


@pytest.mark.parametrize('field,value', [('id', 42), ('engine', None)])
def test_malformed_project_manifest_is_rejected(tmp_path, field, value):
    project = tmp_path / 'bad-app'
    vision.create(project)
    data = json.loads((project / 'project.json').read_text())
    data[field] = value
    (project / 'project.json').write_text(json.dumps(data))
    with pytest.raises(ValueError):
        vision.project_info(project)


def test_each_existing_module_belongs_to_one_project():
    migration = json.loads((ROOT / 'projects/MIGRATION.json').read_text())
    seen = set()
    for name, expected in migration['projects'].items():
        project = ROOT / 'projects' / name
        vision.check(project)
        catalog = vision.build_catalog(project / 'logic')
        for kind in ('channel_logics', 'global_logics'):
            actual = {item['name'] for item in catalog[kind]}
            assert actual == set(expected[kind])
            assert not seen & actual
            seen.update(actual)
    assert len(seen) == 21
