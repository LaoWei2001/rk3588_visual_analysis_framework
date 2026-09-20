from pathlib import Path
import sys
import pytest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'docs/skills/rk3588-feature-wizard/scripts'))
from write_guard import IsolatedLogicWorkspace, WriteBoundaryError


def test_external_project_promotion_preserves_engine_boundary(tmp_path):
    project = tmp_path / 'app'
    module = project / 'logic/modules/logic_test/logic.cpp'
    module.parent.mkdir(parents=True)
    module.write_text('original\n')
    with IsolatedLogicWorkspace(project, support_engine=ROOT) as isolated:
        (isolated.path / 'logic/modules/logic_test/logic.cpp').write_text('updated\n')
        assert isolated.promote().changed_paths == ('logic/modules/logic_test/logic.cpp',)
    assert module.read_text() == 'updated\n'
    with IsolatedLogicWorkspace(project, support_engine=ROOT) as isolated:
        (isolated.path / '.engine/VERSION').write_text('tampered\n')
        with pytest.raises(WriteBoundaryError):
            isolated.promote()
    assert (ROOT / 'VERSION').read_text().strip() == '1.0.0'
