"""Integration gates for a native shared build (set RKVISION_TEST_BUILD)."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest

from tools.rkvision import project_workflow as vision


@pytest.fixture
def shared_build():
    location = os.environ.get('RKVISION_TEST_BUILD')
    if not location:
        pytest.skip('Set RKVISION_TEST_BUILD to a native person_count build directory')
    return Path(location).resolve()


def test_dependencies_under_chinese_locale(shared_build, monkeypatch):
    monkeypatch.setenv('LC_ALL', 'zh_CN.UTF-8')
    dependencies = vision.elf_dependencies(shared_build / 'vision_analysis',
                                           shared_build / 'libs/librkvision.so.1')
    assert 'librkvision.so.1' in dependencies
    assert 'librknnrt.so' in dependencies
    assert any(name.startswith('libopencv_core.') for name in dependencies)


def test_relocated_package_and_runtime_replacement(shared_build, tmp_path):
    package = vision.ENGINE / 'projects/person_count/dist/person-count'
    for dependency in vision.elf_dependencies(package / 'vision_analysis', package / 'libs/librkvision.so.1'):
        if dependency.startswith(('ld-linux', 'libc.so', 'libm.so', 'libdl.so', 'librt.so', 'libpthread.so', 'libstdc++.so')):
            continue
        assert (package / 'libs' / dependency).is_file(), dependency
    relocated = tmp_path / 'app'
    relocated.mkdir()
    shutil.copy2(package / 'vision_analysis', relocated)
    shutil.copytree(package / 'libs', relocated / 'libs')
    environment = dict(os.environ)
    environment.pop('LD_LIBRARY_PATH', None)
    binary = relocated / 'vision_analysis'
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    for replacement in (None, shared_build / 'libs/librkvision.so.1'):
        if replacement:
            shutil.copy2(replacement, relocated / 'libs/librkvision.so.1')
        for option, expected in (
            ('--list-logics', ['logic_roi_person_count_demo']),
            ('--list-global-logics', ['global_person_count_alarm_demo']),
        ):
            output = subprocess.check_output([binary, option], cwd=tmp_path, env=environment, text=True)
            assert output.splitlines() == expected
    assert hashlib.sha256(binary.read_bytes()).hexdigest() == digest
    config = json.loads((vision.ENGINE / 'projects/person_count/assets/config_global.json').read_text())
    channel = next(item for item in config['channels'] if item.get('enable', True))
    channel.setdefault('logic_parameters', {})['unregistered_parameter'] = 42
    invalid = tmp_path / 'invalid.json'
    invalid.write_text(json.dumps(config))
    result = subprocess.run([binary, '--validate-config', invalid], env=environment,
                            cwd=tmp_path, capture_output=True, text=True)
    assert result.returncode != 0
    assert 'unregistered_parameter' in result.stdout + result.stderr


def test_runtime_self_contained_and_abi_rejected(shared_build, tmp_path):
    # Link a C client with no application business/catalog symbols. The runtime
    # must resolve all its references itself and reject mismatches before startup.
    source = tmp_path / 'probe.c'
    source.write_text('''#include <rkvision/application.h>
int main(void) {
    RkVisionApplication app = {999, sizeof(RkVisionApplication), "wrong", "{}"};
    if (rkvision_run(0, 0, 0) != 2) return 1;
    if (rkvision_run(0, 0, &app) != 2) return 2;
    app.abi_version = RKVISION_RUNTIME_ABI;
    if (rkvision_run(0, 0, &app) != 2) return 3;
    return 0;
}
''')
    environment = dict(os.environ)
    environment['LD_LIBRARY_PATH'] = str(shared_build / 'libs') + ':' + str(
        vision.ENGINE / 'vision_analysis/vendor/rknn/2.4.2a2/lib/aarch64')
    binary = tmp_path / 'probe'
    subprocess.run(['cc', '-I'+str(vision.ENGINE / 'vision_analysis/include'), source,
                    shared_build / 'libs/librkvision.so.1', '-o', binary],
                   env=environment, check=True)
    result = subprocess.run([binary], env=environment, capture_output=True, text=True)
    assert result.returncode == 0
    assert result.stderr.count('incompatible application/runtime ABI') == 3
    symbols = subprocess.check_output(['nm', '-D', '-C', str(shared_build / 'libs/librkvision.so.1')], text=True)
    assert 'logic_embedded_catalog_json' not in symbols
    assert 'logic_roi_person_count_demo' not in symbols


def test_cmake_refuses_incompatible_prebuilt_sdk(shared_build, tmp_path):
    runtime = tmp_path / 'runtime'
    shutil.copytree(shared_build / 'libs', runtime)
    metadata = runtime / 'rkvision-runtime.cmake'
    metadata.write_text(re.sub(r'(RKVISION_BUILT_ABI_TAG) "[^"]+"', r'\1 "incompatible"', metadata.read_text()))
    result = subprocess.run(['cmake', '-S', vision.ENGINE / 'projects/person_count',
                             '-B', tmp_path / 'build', '-DCMAKE_BUILD_TYPE=Release',
                             '-DRKVISION_RUNTIME_DIR='+str(runtime)], capture_output=True, text=True)
    assert result.returncode != 0
    assert 'Shared runtime SDK/compiler/version/build type mismatch' in result.stderr
