#!/usr/bin/env python3
"""Isolate agent edits and promote only approved Logic-module changes."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import tempfile
from typing import Mapping, Sequence


ALLOWED_WRITE_ROOTS = (
    PurePosixPath("vision_analysis/src/logic/modules"),
    PurePosixPath("vision_analysis/src/logic/global_modules"),
)


class WriteBoundaryError(RuntimeError):
    """Raised when an isolated session cannot be created or safely promoted."""


@dataclass(frozen=True)
class PromotionResult:
    """Result of copying an allowed patch back to the source repository."""

    changed_paths: tuple[str, ...]


@dataclass(frozen=True)
class FileState:
    """Content identity and Git-relevant mode for one regular file."""

    digest: str
    mode: int


@dataclass(frozen=True)
class TreeSnapshot:
    """Launcher-owned view of a workspace, independent of its Git metadata."""

    files: Mapping[str, FileState]
    allowed_contents: Mapping[str, bytes]


def _run(
    command: Sequence[str],
    cwd: Path,
    *,
    input_bytes: bytes | None = None,
) -> bytes:
    environment = dict(os.environ)
    for key in tuple(environment):
        if key.startswith("GIT_CONFIG_KEY_") or key.startswith("GIT_CONFIG_VALUE_"):
            environment.pop(key, None)
    for key in (
        "GIT_ALTERNATE_OBJECT_DIRECTORIES",
        "GIT_COMMON_DIR",
        "GIT_CONFIG_COUNT",
        "GIT_DIR",
        "GIT_INDEX_FILE",
        "GIT_OBJECT_DIRECTORY",
        "GIT_PREFIX",
        "GIT_WORK_TREE",
    ):
        environment.pop(key, None)
    environment["GIT_CONFIG_GLOBAL"] = os.devnull
    environment["GIT_CONFIG_NOSYSTEM"] = "1"
    environment["GIT_OPTIONAL_LOCKS"] = "0"
    environment["GIT_TERMINAL_PROMPT"] = "0"
    effective_command = list(command)
    if effective_command and Path(effective_command[0]).name.lower() in {"git", "git.exe"}:
        effective_command[1:1] = ("-c", "core.fsmonitor=false")
    try:
        result = subprocess.run(
            effective_command,
            cwd=cwd,
            check=False,
            env=environment,
            input=input_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as exc:
        raise WriteBoundaryError(f"无法执行 {command[0]}：{exc}") from exc
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise WriteBoundaryError(
            f"命令失败（{result.returncode}）：{' '.join(command)}"
            + (f"\n{detail}" if detail else "")
        )
    return result.stdout


def _git_paths(output: bytes) -> tuple[str, ...]:
    return tuple(
        os.fsdecode(item)
        for item in output.split(b"\0")
        if item
    )


def _safe_relative_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or ".." in path.parts:
        raise WriteBoundaryError(f"Git 返回了不安全的仓库路径：{value!r}")
    return path


def is_allowed_write_path(value: str) -> bool:
    """Return whether a repository-relative path is below an approved root."""
    path = _safe_relative_path(value)
    for root in ALLOWED_WRITE_ROOTS:
        if len(path.parts) > len(root.parts) and path.parts[: len(root.parts)] == root.parts:
            return True
    return False


def allowed_roots_text() -> str:
    return "、".join(f"`{root.as_posix()}/**`" for root in ALLOWED_WRITE_ROOTS)


class IsolatedLogicWorkspace:
    """Run an agent in a disposable repository and promote an allowlisted patch."""

    def __init__(self, source_repo: Path):
        self.source_repo = source_repo.resolve()
        self.path: Path | None = None
        self.baseline_revision: str | None = None
        self._baseline_snapshot: TreeSnapshot | None = None
        self._temporary: tempfile.TemporaryDirectory[str] | None = None

    def __enter__(self) -> "IsolatedLogicWorkspace":
        self._temporary = tempfile.TemporaryDirectory(prefix="rk3588-logic-wizard-")
        self.path = Path(self._temporary.name) / "repository"
        self.path.mkdir()
        try:
            self._copy_current_repository()
            self._initialize_baseline()
            self._baseline_snapshot = self._scan_tree(self.path)
        except Exception:
            self._temporary.cleanup()
            self._temporary = None
            self.path = None
            self.baseline_revision = None
            self._baseline_snapshot = None
            raise
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        if self._temporary is not None:
            self._temporary.cleanup()
        self._temporary = None
        self.path = None
        self.baseline_revision = None
        self._baseline_snapshot = None

    def _require_workspace(self) -> tuple[Path, str, TreeSnapshot]:
        if (
            self.path is None
            or self.baseline_revision is None
            or self._baseline_snapshot is None
        ):
            raise WriteBoundaryError("隔离工作副本尚未初始化。")
        return self.path, self.baseline_revision, self._baseline_snapshot

    def _copy_current_repository(self) -> None:
        assert self.path is not None
        output = _run(
            (
                "git",
                "ls-files",
                "--cached",
                "--others",
                "--exclude-standard",
                "-z",
            ),
            self.source_repo,
        )
        for value in sorted(set(_git_paths(output))):
            relative = _safe_relative_path(value)
            self._verify_source_parent_chain(relative)
            source = self.source_repo.joinpath(*relative.parts)
            if not source.exists() and not source.is_symlink():
                continue
            destination = self.path.joinpath(*relative.parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            if source.is_symlink():
                raise WriteBoundaryError(
                    f"隔离副本拒绝 Git 管理范围内的符号链接：{value}"
                )
            elif source.is_file():
                shutil.copy2(source, destination)
            else:
                raise WriteBoundaryError(
                    f"隔离副本暂不支持 Git 子模块或特殊文件：{value}"
                )

    def _initialize_baseline(self) -> None:
        assert self.path is not None
        _run(("git", "init", "--quiet"), self.path)
        _run(("git", "config", "core.autocrlf", "false"), self.path)
        _run(("git", "config", "user.name", "RK3588 Logic Wizard"), self.path)
        _run(("git", "config", "user.email", "wizard@localhost"), self.path)
        _run(("git", "config", "commit.gpgsign", "false"), self.path)
        _run(("git", "add", "--force", "--all"), self.path)
        _run(
            (
                "git",
                "commit",
                "--quiet",
                "--no-verify",
                "--no-gpg-sign",
                "-m",
                "isolated wizard baseline",
            ),
            self.path,
        )
        self.baseline_revision = _run(
            ("git", "rev-parse", "HEAD"), self.path
        ).decode("ascii").strip()

    def _scan_tree(self, root: Path) -> TreeSnapshot:
        """Hash every regular file without consulting agent-controlled Git state."""
        files: dict[str, FileState] = {}
        allowed_contents: dict[str, bytes] = {}

        def visit(directory: Path, parts: tuple[str, ...]) -> None:
            try:
                entries = sorted(os.scandir(directory), key=lambda item: item.name)
            except OSError as exc:
                raise WriteBoundaryError(f"无法扫描隔离副本 {directory}：{exc}") from exc
            for entry in entries:
                if not parts and entry.name == ".git":
                    # The agent may use Git for normal work, but promotion never trusts
                    # or propagates its disposable repository metadata.
                    continue
                relative_parts = (*parts, entry.name)
                relative = PurePosixPath(*relative_parts).as_posix()
                try:
                    metadata = entry.stat(follow_symlinks=False)
                except OSError as exc:
                    raise WriteBoundaryError(f"无法检查隔离路径 {relative}：{exc}") from exc
                if stat.S_ISLNK(metadata.st_mode):
                    raise WriteBoundaryError(f"隔离副本不允许符号链接：{relative}")
                if stat.S_ISDIR(metadata.st_mode):
                    visit(Path(entry.path), relative_parts)
                    continue
                if not stat.S_ISREG(metadata.st_mode):
                    raise WriteBoundaryError(f"隔离副本不允许特殊文件：{relative}")
                try:
                    content = Path(entry.path).read_bytes()
                except OSError as exc:
                    raise WriteBoundaryError(f"无法读取隔离文件 {relative}：{exc}") from exc
                mode = 0o755 if metadata.st_mode & 0o111 else 0o644
                files[relative] = FileState(
                    digest=hashlib.sha256(content).hexdigest(),
                    mode=mode,
                )
                if is_allowed_write_path(relative):
                    allowed_contents[relative] = content

        visit(root, ())
        return TreeSnapshot(files=files, allowed_contents=allowed_contents)

    def child_environment(self) -> Mapping[str, str]:
        workspace, _, _ = self._require_workspace()
        environment = dict(os.environ)
        for key in (
            "OLDPWD",
            "INIT_CWD",
            "GIT_DIR",
            "GIT_WORK_TREE",
            "GIT_PREFIX",
            "CLAUDE_PROJECT_DIR",
        ):
            environment.pop(key, None)
        environment["PWD"] = str(workspace)
        environment["RK3588_WIZARD_ISOLATED"] = "1"
        environment["RK3588_WIZARD_WRITE_ROOTS"] = ";".join(
            root.as_posix() for root in ALLOWED_WRITE_ROOTS
        )
        return environment

    @staticmethod
    def _changed_paths(before: TreeSnapshot, after: TreeSnapshot) -> tuple[str, ...]:
        candidates = set(before.files) | set(after.files)
        return tuple(
            sorted(path for path in candidates if before.files.get(path) != after.files.get(path))
        )

    @staticmethod
    def _write_regular_file(path: Path, content: bytes, mode: int) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(mode)

    def _build_trusted_patch(
        self,
        after: TreeSnapshot,
        changed_paths: Sequence[str],
    ) -> bytes:
        """Create a patch in fresh Git metadata after the agent has exited."""
        _, _, before = self._require_workspace()
        assert self._temporary is not None
        patch_repo = Path(
            tempfile.mkdtemp(prefix="trusted-patch-", dir=self._temporary.name)
        )

        for path, content in before.allowed_contents.items():
            state = before.files[path]
            self._write_regular_file(patch_repo / path, content, state.mode)

        _run(("git", "init", "--quiet"), patch_repo)
        _run(("git", "config", "core.autocrlf", "false"), patch_repo)
        _run(("git", "config", "core.hooksPath", ".git/no-hooks"), patch_repo)
        _run(("git", "config", "user.name", "RK3588 Logic Wizard"), patch_repo)
        _run(("git", "config", "user.email", "wizard@localhost"), patch_repo)
        _run(("git", "config", "commit.gpgsign", "false"), patch_repo)
        _run(("git", "add", "--force", "--all"), patch_repo)
        _run(
            (
                "git",
                "commit",
                "--quiet",
                "--allow-empty",
                "--no-verify",
                "--no-gpg-sign",
                "-m",
                "trusted promotion baseline",
            ),
            patch_repo,
        )
        trusted_baseline = _run(("git", "rev-parse", "HEAD"), patch_repo).decode(
            "ascii"
        ).strip()

        for root in ALLOWED_WRITE_ROOTS:
            target = patch_repo.joinpath(*root.parts)
            if target.exists():
                shutil.rmtree(target)
        for path, content in after.allowed_contents.items():
            state = after.files[path]
            self._write_regular_file(patch_repo / path, content, state.mode)

        _run(("git", "add", "--force", "--all"), patch_repo)
        patch_paths = tuple(
            sorted(
                _git_paths(
                    _run(
                        (
                            "git",
                            "diff",
                            "--cached",
                            "--name-only",
                            "--no-renames",
                            "-z",
                            trusted_baseline,
                        ),
                        patch_repo,
                    )
                )
            )
        )
        if patch_paths != tuple(changed_paths):
            raise WriteBoundaryError(
                "可信补丁路径与文件扫描结果不一致，拒绝回写。"
            )
        return _run(
            (
                "git",
                "diff",
                "--cached",
                "--binary",
                "--full-index",
                "--no-renames",
                trusted_baseline,
            ),
            patch_repo,
        )

    @staticmethod
    def _source_file_state(path: Path) -> FileState | None:
        if not path.exists() and not path.is_symlink():
            return None
        try:
            metadata = path.lstat()
        except OSError as exc:
            raise WriteBoundaryError(f"无法检查原仓库路径 {path}：{exc}") from exc
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            raise WriteBoundaryError(f"原仓库目标不是普通文件，拒绝覆盖：{path}")
        try:
            content = path.read_bytes()
        except OSError as exc:
            raise WriteBoundaryError(f"无法读取原仓库文件 {path}：{exc}") from exc
        mode = 0o755 if metadata.st_mode & 0o111 else 0o644
        return FileState(hashlib.sha256(content).hexdigest(), mode)

    def _verify_source_parent_chain(self, relative: PurePosixPath) -> None:
        current = self.source_repo
        for part in relative.parts[:-1]:
            current /= part
            if not current.exists() and not current.is_symlink():
                return
            try:
                metadata = current.lstat()
            except OSError as exc:
                raise WriteBoundaryError(f"无法检查原仓库父路径 {current}：{exc}") from exc
            if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
                raise WriteBoundaryError(f"原仓库父路径不是普通目录，拒绝访问：{current}")

    def _verify_source_unchanged(
        self,
        before: TreeSnapshot,
        changed_paths: Sequence[str],
    ) -> None:
        for value in changed_paths:
            relative = _safe_relative_path(value)
            self._verify_source_parent_chain(relative)
            target = self.source_repo.joinpath(*relative.parts)
            if self._source_file_state(target) != before.files.get(value):
                raise WriteBoundaryError(
                    f"原仓库文件在会话期间已被其他进程修改，拒绝覆盖：{value}"
                )

    def _verify_applied_result(
        self,
        after: TreeSnapshot,
        changed_paths: Sequence[str],
    ) -> None:
        for value in changed_paths:
            relative = _safe_relative_path(value)
            self._verify_source_parent_chain(relative)
            target = self.source_repo.joinpath(*relative.parts)
            if self._source_file_state(target) != after.files.get(value):
                raise WriteBoundaryError(f"回写后内容校验失败：{value}")

    def promote(self) -> PromotionResult:
        """Apply only Logic-module changes to the source repository atomically."""
        workspace, _, before = self._require_workspace()
        after = self._scan_tree(workspace)
        changed_paths = self._changed_paths(before, after)
        outside = [path for path in changed_paths if not is_allowed_write_path(path)]
        if outside:
            raise WriteBoundaryError(
                "隔离副本出现白名单外改动；本次所有改动均未回写。越界路径："
                + "、".join(outside[:20])
            )
        if not changed_paths:
            return PromotionResult(())

        self._verify_source_unchanged(before, changed_paths)
        patch = self._build_trusted_patch(after, changed_paths)
        if not patch:
            raise WriteBoundaryError("检测到改动但未能生成可信补丁，拒绝回写。")

        apply_command = (
            "git",
            "-c",
            "core.autocrlf=false",
            "-c",
            "core.safecrlf=false",
            "apply",
            "--binary",
            "--whitespace=nowarn",
            "-",
        )
        check_command = (*apply_command[:-1], "--check", "-")
        _run(check_command, self.source_repo, input_bytes=patch)
        _run(apply_command, self.source_repo, input_bytes=patch)
        self._verify_applied_result(after, changed_paths)
        return PromotionResult(changed_paths)
