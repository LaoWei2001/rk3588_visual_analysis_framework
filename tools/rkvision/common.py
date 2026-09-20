"""Shared RKVision CLI context, target discovery and project workflow access."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import sys
from typing import Iterable

from . import project_workflow as PROJECT_WORKFLOW


ENGINE_ROOT = Path(__file__).resolve().parents[2]


class CommandError(RuntimeError):
    """A user-facing command failure without a Python traceback."""


@dataclass(frozen=True)
class ProjectTarget:
    name: str
    app_id: str
    kind: str
    path: Path
    version: str


def _read_manifest(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise CommandError(
            f"项目清单 JSON 格式错误 {path}（第 {exc.lineno} 行，第 {exc.colno} 列）"
        ) from exc
    except OSError as exc:
        raise CommandError(f"无法读取项目清单：{path}") from exc
    if not isinstance(value, dict):
        raise CommandError(f"项目清单不是 JSON 对象：{path}")
    return value


def discover_projects() -> list[ProjectTarget]:
    targets: list[ProjectTarget] = []
    for root, default_kind in (
        (ENGINE_ROOT / "projects", "app"),
        (ENGINE_ROOT / "examples", "example"),
    ):
        if not root.is_dir():
            continue
        for manifest_path in sorted(root.glob("*/project.json")):
            data = _read_manifest(manifest_path)
            directory_name = manifest_path.parent.name
            kind = str(data.get("kind") or default_kind)
            if directory_name.startswith("course_"):
                kind = "example"
            targets.append(
                ProjectTarget(
                    name=directory_name,
                    app_id=str(data.get("id") or directory_name),
                    kind=kind,
                    path=manifest_path.parent.resolve(),
                    version=str(data.get("version") or ""),
                )
            )
    return targets


def _project_from_path(path: Path) -> ProjectTarget:
    manifest_path = path / "project.json"
    if not manifest_path.is_file():
        raise CommandError(f"不是 RKVision 项目，缺少：{manifest_path}")
    data = _read_manifest(manifest_path)
    return ProjectTarget(
        name=path.name,
        app_id=str(data.get("id") or path.name),
        kind=str(data.get("kind") or "external"),
        path=path.resolve(),
        version=str(data.get("version") or ""),
    )


def _project_in_parents(start: Path) -> ProjectTarget | None:
    current = start.resolve()
    for candidate in (current, *current.parents):
        if (candidate / "project.json").is_file():
            return _project_from_path(candidate)
    return None


def resolve_project(value: str | Path | None) -> ProjectTarget:
    if value is None:
        current = _project_in_parents(Path.cwd())
        if current is not None:
            return current
        raise CommandError("当前目录不在视觉项目中，请指定项目名；可用 rkvision list 查看")

    text = str(value)
    path = Path(text).expanduser()
    if path.exists() or path.is_absolute() or "/" in text or "\\" in text or text.startswith("."):
        return _project_from_path(path.resolve())

    matches = [target for target in discover_projects() if text in (target.name, target.app_id)]
    if not matches:
        raise CommandError(f"找不到项目 {text!r}；可用 rkvision list 查看")
    if len(matches) > 1:
        choices = ", ".join(str(target.path) for target in matches)
        raise CommandError(f"项目名 {text!r} 不唯一，请改用路径：{choices}")
    return matches[0]


def select_projects(target: str | None, all_targets: bool, kind: str | None) -> list[ProjectTarget]:
    if all_targets:
        if target is not None:
            raise CommandError("项目名和 --all 不能同时使用")
        targets = discover_projects()
        if kind:
            targets = [item for item in targets if item.kind == kind]
        if not targets:
            raise CommandError("没有匹配的项目")
        return targets
    project = resolve_project(target)
    if kind and project.kind != kind:
        raise CommandError(f"{project.name} 的类型是 {project.kind}，不是 {kind}")
    return [project]


def create_destination(value: str) -> Path:
    path = Path(value).expanduser()
    if path.is_absolute() or "/" in value or "\\" in value or value.startswith("."):
        return path.resolve()
    return (ENGINE_ROOT / "projects" / value).resolve()


PROFILE_TO_CMAKE = {
    "debug": "Debug",
    "release": "Release",
    "relwithdebinfo": "RelWithDebInfo",
}


def add_build_arguments(parser: argparse.ArgumentParser, *, clean: bool = True) -> None:
    parser.add_argument(
        "--profile",
        choices=tuple(PROFILE_TO_CMAKE),
        default="release",
        help="构建配置，默认 release",
    )
    parser.add_argument("-j", "--jobs", type=int, default=min(os.cpu_count() or 1, 4))
    if clean:
        parser.add_argument("--clean", action="store_true", help="构建前清理当前配置缓存")
    compiler = parser.add_mutually_exclusive_group()
    compiler.add_argument("--toolchain", type=Path)
    compiler.add_argument("--image", help="交叉编译 Docker 镜像")


def prepare_build_args(args: argparse.Namespace) -> argparse.Namespace:
    args.build_type = PROFILE_TO_CMAKE[args.profile]
    if not hasattr(args, "clean"):
        args.clean = False
    return args


def run_for_projects(
    projects: Iterable[ProjectTarget],
    operation,
    *,
    continue_on_error: bool = False,
) -> int:
    failures: list[tuple[ProjectTarget, Exception]] = []
    for project in projects:
        try:
            operation(project)
        except Exception as exc:
            if not continue_on_error:
                raise
            failures.append((project, exc))
            print(f"[失败] {project.name}: {exc}", file=sys.stderr)
    if failures:
        print(f"完成，但有 {len(failures)} 个目标失败", file=sys.stderr)
        return 1
    return 0
