import json
from pathlib import Path
import subprocess

import pytest

from tools.rkvision import cli
from tools.rkvision.common import ENGINE_ROOT, resolve_project
from tools.rkvision.commands import install_cmd
from tools.rkvision.commands.run_cmd import _runtime_config


def test_project_can_be_selected_by_directory_name_or_application_id():
    by_name = resolve_project("person_count")
    by_id = resolve_project("person-count")
    assert by_name.path == by_id.path == ENGINE_ROOT / "projects/person_count"


def test_list_json_is_machine_readable(capsys):
    assert cli.main(["list", "--type", "app", "--json"]) == 0
    values = json.loads(capsys.readouterr().out)
    assert {item["name"] for item in values} >= {"person_count", "object_detection"}
    assert all(item["type"] == "app" for item in values)


def test_each_command_module_starts_with_usage():
    for module in cli.COMMAND_MODULES:
        source = Path(module.__file__).read_text(encoding="utf-8")
        assert source.startswith(('"""Usage:', "'''Usage:")), module.__name__


def test_workspace_projects_do_not_copy_build_launchers():
    assert not list((ENGINE_ROOT / "projects").glob("*/build.sh"))
    assert not (ENGINE_ROOT / "templates/basic_project/build.sh").exists()


def test_legacy_command_entrypoints_are_removed():
    assert not (ENGINE_ROOT / "vision").exists()
    assert not (ENGINE_ROOT / "vision.cmd").exists()


@pytest.mark.parametrize(
    "argv",
    (
        ["new-logic", "person_count", "channel", "logic_old"],
        ["build", "person_count", "--build-type", "Debug"],
        ["develop", "--provider", "codex"],
    ),
)
def test_legacy_command_syntax_is_rejected(argv):
    with pytest.raises(SystemExit) as result:
        cli.main(argv)
    assert result.value.code == 2


def test_display_override_does_not_modify_project_config(tmp_path):
    source = tmp_path / "config.json"
    source.write_text('{"global":{"enable_display":0}}\n', encoding="utf-8")
    runtime, temporary = _runtime_config(source, True)
    try:
        assert json.loads(source.read_text(encoding="utf-8"))["global"]["enable_display"] == 0
        assert json.loads(runtime.read_text(encoding="utf-8"))["global"]["enable_display"] == 1
    finally:
        assert temporary is not None
        temporary.unlink(missing_ok=True)


def test_create_and_logic_add_use_unified_command(tmp_path):
    project = tmp_path / "cli-app"
    subprocess.run([str(ENGINE_ROOT / "rkvision"), "create", str(project)], check=True)
    subprocess.run(
        [str(ENGINE_ROOT / "rkvision"), "logic", "add", "global", "global_cli", str(project)],
        check=True,
    )
    assert (project / "logic/global_modules/global_cli/logic.cpp").is_file()


@pytest.mark.parametrize(
    "command",
    (
        "list", "create", "check", "build", "run", "start", "stop", "restart",
        "status", "logs", "autostart", "package", "clean", "logic", "install",
        "doctor", "bench", "platform", "hardware", "network", "develop",
    ),
)
def test_each_command_has_help(command):
    parser = cli.build_parser()
    with pytest.raises(SystemExit) as result:
        parser.parse_args([command, "--help"])
    assert result.value.code == 0


@pytest.mark.parametrize(
    ("argv", "message"),
    (
        (["start"], "缺少必填参数"),
        (["build", "person_count", "--profile", "fast"], "无效，可选值"),
        (["build", "person_count", "--unknown"], "无法识别的参数"),
        (["build", "person_count", "-j", "many"], "不是有效的整数"),
    ),
)
def test_argument_errors_are_chinese(argv, message, capsys):
    with pytest.raises(SystemExit) as result:
        cli.main(argv)
    captured = capsys.readouterr()
    assert result.value.code == 2
    assert "用法：" in captured.err
    assert "：错误：" in captured.err
    assert message in captured.err
    assert "usage:" not in captured.err
    assert ": error:" not in captured.err


def test_project_validation_error_is_chinese(tmp_path, capsys):
    project = tmp_path / "bad-schema"
    assert cli.main(["create", str(project)]) == 0
    manifest = project / "logic/modules/logic_example/logic.json"
    value = json.loads(manifest.read_text(encoding="utf-8"))
    value["parameters"] = []
    manifest.write_text(json.dumps(value), encoding="utf-8")

    assert cli.main(["check", str(project)]) == 1
    error = capsys.readouterr().err
    assert "错误：" in error
    assert "parameters 必须是 JSON Schema 对象" in error


def test_existing_destination_error_is_chinese(tmp_path, capsys):
    destination = tmp_path / "existing"
    destination.mkdir()
    assert cli.main(["create", str(destination)]) == 1
    assert "错误：目标目录已存在" in capsys.readouterr().err


def test_install_resolves_project_name_and_packages_automatically(monkeypatch, tmp_path, capsys):
    generated = tmp_path / "person-count"
    (generated / "assets").mkdir(parents=True)
    (generated / "vision_analysis").touch()
    calls = {}

    def fake_package(project, args):
        calls["project"] = project
        calls["profile"] = args.build_type
        return generated

    def fake_install(package, **kwargs):
        calls["package"] = package
        calls.update(kwargs)

    monkeypatch.setattr(install_cmd.PROJECT_WORKFLOW, "package", fake_package)
    monkeypatch.setattr(install_cmd, "install", fake_install)

    args = cli.build_parser().parse_args(["install", "person_count"])
    assert args.command_handler(args) == 0
    assert calls == {
        "project": ENGINE_ROOT / "projects/person_count",
        "profile": "Release",
        "package": generated,
        "name": None,
        "replace_assets": False,
    }
    assert "已找到项目" in capsys.readouterr().out


def test_install_accepts_application_id(monkeypatch, tmp_path):
    generated = tmp_path / "person-count"
    monkeypatch.setattr(
        install_cmd.PROJECT_WORKFLOW,
        "package",
        lambda project, args: generated,
    )
    installed = []
    monkeypatch.setattr(install_cmd, "install", lambda package, **kwargs: installed.append(package))

    args = cli.build_parser().parse_args(["install", "person-count"])
    assert args.command_handler(args) == 0
    assert installed == [generated]


def test_install_uses_complete_package_directory_without_rebuild(monkeypatch, tmp_path):
    package = tmp_path / "ready-package"
    (package / "assets").mkdir(parents=True)
    (package / "vision_analysis").touch()
    monkeypatch.setattr(
        install_cmd.PROJECT_WORKFLOW,
        "package",
        lambda *_: pytest.fail("完整发布包不应重新构建"),
    )
    installed = []
    monkeypatch.setattr(install_cmd, "install", lambda source, **kwargs: installed.append(source))

    args = cli.build_parser().parse_args(["install", str(package)])
    assert args.command_handler(args) == 0
    assert installed == [package.resolve()]
