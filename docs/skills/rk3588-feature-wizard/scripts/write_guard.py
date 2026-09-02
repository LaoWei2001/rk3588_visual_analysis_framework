#!/usr/bin/env python3
"""Isolate agent edits and promote only approved Logic-module file changes."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tempfile
from typing import Mapping, Sequence


ALLOWED_WRITE_ROOTS = (
    PurePosixPath("vision_analysis/src/logic/modules"),
    PurePosixPath("vision_analysis/src/logic/global_modules"),
)

# Keep generated dependencies, editor state, and large media out of the disposable
# copy without consulting Git metadata. The finished workspace is still scanned in
# full, so an agent-created file under one of these names cannot bypass promotion.
SOURCE_COPY_EXCLUDED_DIRECTORY_NAMES = frozenset(
    {
        ".git",
        ".claude",
        ".vscode",
        "__pycache__",
        "build",
        "dist",
        "node_modules",
    }
)
SOURCE_COPY_EXCLUDED_SUFFIXES = (".docx", ".mp4", ".tsbuildinfo")
SOURCE_COPY_EXCLUDED_ROOT_FILES = frozenset({"librknnrt.so"})


class WriteBoundaryError(RuntimeError):
    """Raised when an isolated session cannot be created or safely promoted."""


@dataclass(frozen=True)
class PromotionResult:
    """Result of copying allowed file changes back to the source project."""

    changed_paths: tuple[str, ...]


@dataclass(frozen=True)
class FileState:
    """Content identity and executable/non-executable mode for a regular file."""

    digest: str
    mode: int


@dataclass(frozen=True)
class TreeSnapshot:
    """Launcher-owned content view of a project directory."""

    files: Mapping[str, FileState]
    allowed_contents: Mapping[str, bytes]


def _safe_relative_path(value: str) -> PurePosixPath:
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or ".." in path.parts:
        raise WriteBoundaryError(f"检测到不安全的项目相对路径：{value!r}")
    return path


def is_allowed_write_path(value: str) -> bool:
    """Return whether a project-relative path is below an approved root."""
    path = _safe_relative_path(value)
    if ".git" in path.parts:
        return False
    for root in ALLOWED_WRITE_ROOTS:
        if len(path.parts) > len(root.parts) and path.parts[: len(root.parts)] == root.parts:
            return True
    return False


def allowed_roots_text() -> str:
    return "、".join(f"`{root.as_posix()}/**`" for root in ALLOWED_WRITE_ROOTS)


class IsolatedLogicWorkspace:
    """Run an agent in a disposable project copy and promote allowlisted files."""

    def __init__(self, source_repo: Path):
        self.source_repo = source_repo.resolve()
        self.path: Path | None = None
        self._baseline_snapshot: TreeSnapshot | None = None
        self._temporary: tempfile.TemporaryDirectory[str] | None = None

    def __enter__(self) -> "IsolatedLogicWorkspace":
        self._temporary = tempfile.TemporaryDirectory(prefix="rk3588-logic-wizard-")
        self.path = Path(self._temporary.name) / "repository"
        self.path.mkdir()
        try:
            self._copy_current_project()
            self._baseline_snapshot = self._scan_tree(self.path)
        except Exception:
            self._temporary.cleanup()
            self._temporary = None
            self.path = None
            self._baseline_snapshot = None
            raise
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        if self._temporary is not None:
            self._temporary.cleanup()
        self._temporary = None
        self.path = None
        self._baseline_snapshot = None

    def _require_workspace(self) -> tuple[Path, TreeSnapshot]:
        if self.path is None or self._baseline_snapshot is None:
            raise WriteBoundaryError("隔离工作副本尚未初始化。")
        return self.path, self._baseline_snapshot

    @staticmethod
    def _source_copy_excluded(relative: PurePosixPath, is_directory: bool) -> bool:
        if is_allowed_write_path(relative.as_posix()):
            return False
        if relative.name == ".git":
            return True
        if is_directory and relative.name in SOURCE_COPY_EXCLUDED_DIRECTORY_NAMES:
            return True
        if len(relative.parts) == 1 and relative.name in SOURCE_COPY_EXCLUDED_ROOT_FILES:
            return True
        if not is_directory and relative.name.endswith(SOURCE_COPY_EXCLUDED_SUFFIXES):
            return True
        if not is_directory and relative.name.endswith("_queue_producer_test"):
            return True
        return False

    def _copy_current_project(self) -> None:
        """Copy project files without requiring or reading any Git metadata."""
        assert self.path is not None

        def copy_directory(directory: Path, parts: tuple[str, ...]) -> None:
            try:
                entries = sorted(os.scandir(directory), key=lambda item: item.name)
            except OSError as exc:
                raise WriteBoundaryError(f"无法读取项目目录 {directory}：{exc}") from exc
            for entry in entries:
                relative_parts = (*parts, entry.name)
                relative = PurePosixPath(*relative_parts)
                try:
                    metadata = entry.stat(follow_symlinks=False)
                except OSError as exc:
                    raise WriteBoundaryError(
                        f"无法检查项目路径 {relative.as_posix()}：{exc}"
                    ) from exc
                if stat.S_ISLNK(metadata.st_mode):
                    if self._source_copy_excluded(
                        relative,
                        entry.is_dir(follow_symlinks=False),
                    ):
                        continue
                    raise WriteBoundaryError(
                        f"隔离副本不允许项目中的符号链接：{relative.as_posix()}"
                    )
                if stat.S_ISDIR(metadata.st_mode):
                    if self._source_copy_excluded(relative, True):
                        continue
                    destination = self.path.joinpath(*relative.parts)
                    destination.mkdir(parents=True, exist_ok=True)
                    copy_directory(Path(entry.path), relative_parts)
                    continue
                if not stat.S_ISREG(metadata.st_mode):
                    raise WriteBoundaryError(
                        f"隔离副本不允许项目中的特殊文件：{relative.as_posix()}"
                    )
                if self._source_copy_excluded(relative, False):
                    continue
                destination = self.path.joinpath(*relative.parts)
                destination.parent.mkdir(parents=True, exist_ok=True)
                try:
                    shutil.copy2(entry.path, destination)
                except OSError as exc:
                    raise WriteBoundaryError(
                        f"无法复制项目文件 {relative.as_posix()}：{exc}"
                    ) from exc

        copy_directory(self.source_repo, ())

    def _scan_tree(self, root: Path) -> TreeSnapshot:
        """Hash every regular file in a launcher-controlled project copy."""
        files: dict[str, FileState] = {}
        allowed_contents: dict[str, bytes] = {}

        def visit(directory: Path, parts: tuple[str, ...]) -> None:
            try:
                entries = sorted(os.scandir(directory), key=lambda item: item.name)
            except OSError as exc:
                raise WriteBoundaryError(f"无法扫描隔离副本 {directory}：{exc}") from exc
            for entry in entries:
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
        workspace, _ = self._require_workspace()
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
    def _source_file_state(path: Path) -> FileState | None:
        if not path.exists() and not path.is_symlink():
            return None
        try:
            metadata = path.lstat()
        except OSError as exc:
            raise WriteBoundaryError(f"无法检查原项目路径 {path}：{exc}") from exc
        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(metadata.st_mode):
            raise WriteBoundaryError(f"原项目目标不是普通文件，拒绝覆盖：{path}")
        try:
            content = path.read_bytes()
        except OSError as exc:
            raise WriteBoundaryError(f"无法读取原项目文件 {path}：{exc}") from exc
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
                raise WriteBoundaryError(f"无法检查原项目父路径 {current}：{exc}") from exc
            if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(metadata.st_mode):
                raise WriteBoundaryError(f"原项目父路径不是普通目录，拒绝访问：{current}")

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
                    f"原项目文件在会话期间已被其他进程修改，拒绝覆盖：{value}"
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

    def _ensure_source_parent_directories(
        self,
        relative: PurePosixPath,
    ) -> tuple[Path, ...]:
        """Create missing target parents while rejecting links and path redirection."""
        created: list[Path] = []
        current = self.source_repo
        try:
            for part in relative.parts[:-1]:
                current /= part
                if not current.exists() and not current.is_symlink():
                    try:
                        current.mkdir()
                    except FileExistsError:
                        pass
                    except OSError as exc:
                        raise WriteBoundaryError(
                            f"无法创建回写目录 {current}：{exc}"
                        ) from exc
                    else:
                        created.append(current)
                try:
                    metadata = current.lstat()
                except OSError as exc:
                    raise WriteBoundaryError(
                        f"无法检查回写目录 {current}：{exc}"
                    ) from exc
                if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISDIR(
                    metadata.st_mode
                ):
                    raise WriteBoundaryError(
                        f"回写父路径不是普通目录，拒绝访问：{current}"
                    )
        except Exception:
            self._cleanup_created_directories(created)
            raise
        return tuple(created)

    @staticmethod
    def _stage_file(target: Path, content: bytes, mode: int) -> Path:
        descriptor, raw_path = tempfile.mkstemp(
            prefix=".rk3588-wizard-",
            suffix=".tmp",
            dir=str(target.parent),
        )
        staged = Path(raw_path)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            staged.chmod(mode)
        except Exception:
            try:
                os.close(descriptor)
            except OSError:
                pass
            try:
                staged.unlink()
            except OSError:
                pass
            raise
        return staged

    @staticmethod
    def _cleanup_paths(paths: Sequence[Path]) -> None:
        for path in paths:
            try:
                path.unlink()
            except FileNotFoundError:
                continue
            except OSError:
                continue

    @staticmethod
    def _cleanup_created_directories(paths: Sequence[Path]) -> None:
        for path in reversed(paths):
            try:
                path.rmdir()
            except OSError:
                continue

    def _rollback_applied_paths(
        self,
        before: TreeSnapshot,
        applied_paths: Sequence[str],
    ) -> tuple[str, ...]:
        failures: list[str] = []
        for value in reversed(applied_paths):
            relative = _safe_relative_path(value)
            target = self.source_repo.joinpath(*relative.parts)
            staged: Path | None = None
            try:
                self._verify_source_parent_chain(relative)
                previous = before.files.get(value)
                if previous is None:
                    if target.exists() or target.is_symlink():
                        metadata = target.lstat()
                        if stat.S_ISLNK(metadata.st_mode) or not stat.S_ISREG(
                            metadata.st_mode
                        ):
                            raise OSError("目标已变为非普通文件")
                        target.unlink()
                else:
                    staged = self._stage_file(
                        target,
                        before.allowed_contents[value],
                        previous.mode,
                    )
                    os.replace(staged, target)
                    staged = None
                    target.chmod(previous.mode)
            except (KeyError, OSError, WriteBoundaryError) as exc:
                failures.append(f"{value}: {exc}")
            finally:
                if staged is not None:
                    self._cleanup_paths((staged,))
        return tuple(failures)

    def _prune_deleted_directories(self, value: str) -> None:
        relative = _safe_relative_path(value)
        matching_root = next(
            root
            for root in ALLOWED_WRITE_ROOTS
            if relative.parts[: len(root.parts)] == root.parts
        )
        stop = self.source_repo.joinpath(*matching_root.parts)
        current = self.source_repo.joinpath(*relative.parts).parent
        while current != stop:
            try:
                current.rmdir()
            except OSError:
                break
            current = current.parent

    def promote(self) -> PromotionResult:
        """Copy only verified Logic-module contents back to the source project."""
        workspace, before = self._require_workspace()
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
        staged_files: dict[str, Path] = {}
        created_directories: list[Path] = []
        applied_paths: list[str] = []
        try:
            for value in changed_paths:
                relative = _safe_relative_path(value)
                if value not in after.files:
                    continue
                created_directories.extend(
                    self._ensure_source_parent_directories(relative)
                )
                target = self.source_repo.joinpath(*relative.parts)
                staged_files[value] = self._stage_file(
                    target,
                    after.allowed_contents[value],
                    after.files[value].mode,
                )

            # Recheck after staging so a concurrent source edit cannot be silently
            # overwritten merely because staging took time.
            self._verify_source_unchanged(before, changed_paths)
            for value in changed_paths:
                relative = _safe_relative_path(value)
                target = self.source_repo.joinpath(*relative.parts)
                expected_before = before.files.get(value)
                self._verify_source_parent_chain(relative)
                if self._source_file_state(target) != expected_before:
                    raise WriteBoundaryError(
                        f"原项目文件在回写前发生变化，拒绝覆盖：{value}"
                    )
                if value in after.files:
                    os.replace(staged_files[value], target)
                    applied_paths.append(value)
                    target.chmod(after.files[value].mode)
                else:
                    target.unlink()
                    applied_paths.append(value)
            self._verify_applied_result(after, changed_paths)
        except Exception as exc:
            rollback_failures = self._rollback_applied_paths(before, applied_paths)
            self._cleanup_paths(tuple(staged_files.values()))
            self._cleanup_created_directories(created_directories)
            if rollback_failures:
                raise WriteBoundaryError(
                    f"回写失败且未能完全恢复原内容：{exc}；回滚错误："
                    + "；".join(rollback_failures)
                ) from exc
            if isinstance(exc, WriteBoundaryError):
                raise
            raise WriteBoundaryError(f"回写失败，原内容已恢复：{exc}") from exc
        finally:
            self._cleanup_paths(tuple(staged_files.values()))

        for value in changed_paths:
            if value not in after.files:
                self._prune_deleted_directories(value)
        return PromotionResult(changed_paths)
