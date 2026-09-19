from pathlib import Path
import pytest
from services.package_install import replace_application


def package(path, config, model):
    (path / 'assets').mkdir(parents=True)
    (path / 'vision_analysis').write_text('binary')
    (path / 'assets/config.json').write_text(config)
    (path / 'assets/model.rknn').write_text(model)
    return path


def test_upgrade_retains_field_configuration_models_and_data(tmp_path):
    old = package(tmp_path / 'app', 'field config', 'OTA model')
    (old / 'run.config').write_text('production.json')
    (old / 'assets/production.json').write_text('production')
    (old / 'obsolete-code.py').write_text('old')
    data = tmp_path / '.data/app/event_store'
    data.mkdir(parents=True)
    (data / 'event.json').write_text('pending event')
    new = package(tmp_path / 'staged', 'default config', 'default model')
    (new / 'assets/new-model.rknn').write_text('new asset')
    replace_application(new, old)
    assert (old / 'assets/config.json').read_text() == 'field config'
    assert (old / 'assets/model.rknn').read_text() == 'OTA model'
    assert (old / 'assets/new-model.rknn').exists()
    assert (old / 'run.config').read_text() == 'production.json'
    assert not (old / 'obsolete-code.py').exists()
    assert (data / 'event.json').read_text() == 'pending event'


def test_explicit_asset_replacement(tmp_path):
    old = package(tmp_path / 'app', 'old', 'old')
    new = package(tmp_path / 'staged', 'new', 'new')
    replace_application(new, old, preserve_assets=False)
    assert (old / 'assets/config.json').read_text() == 'new'


def test_failed_swap_restores_previous_application(tmp_path, monkeypatch):
    old = package(tmp_path / 'app', 'old', 'old')
    new = package(tmp_path / 'staged', 'new', 'new')
    rename = Path.rename
    def fail_staging(self, target):
        if self == new:
            raise OSError('simulated failure')
        return rename(self, target)
    monkeypatch.setattr(Path, 'rename', fail_staging)
    with pytest.raises(OSError, match='simulated'):
        replace_application(new, old)
    assert (old / 'assets/config.json').read_text() == 'old'


def test_invalid_package_does_not_replace_old_application(tmp_path):
    old = package(tmp_path / 'app', 'old', 'old')
    new = tmp_path / 'staged'
    new.mkdir()
    with pytest.raises(ValueError):
        replace_application(new, old)
    assert (old / 'vision_analysis').exists()


def test_shared_runtime_upgrades_together_with_application(tmp_path):
    old = package(tmp_path / 'app', 'field config', 'field model')
    new = package(tmp_path / 'staged', 'default config', 'default model')
    for target, version in ((old, 'old'), (new, 'new')):
        (target / 'libs').mkdir()
        (target / 'libs/librkvision.so.1').write_text(version + ' engine')
        (target / 'vision_analysis').write_text(version + ' application')
    replace_application(new, old)
    assert (old / 'vision_analysis').read_text() == 'new application'
    assert (old / 'libs/librkvision.so.1').read_text() == 'new engine'
    assert (old / 'assets/config.json').read_text() == 'field config'
