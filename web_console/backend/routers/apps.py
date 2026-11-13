"""
apps.py — 程序(App)列表 + 网页上传/删除程序包。

上传/删除做的就是根目录 install_app.sh 的网页版：把一个打包好的程序（根 build.sh 产出的 dist）：
二进制 + assets/ + libs/ + services/ ...)解压进 /opt/ai_apps/<name>/，或反向删除。
命令行 install_app.sh 与网页写入同一处，互不冲突。

安全：
  - 程序名禁止 / .. 及 . / _ 开头(后者保留给 _console / 隐藏目录)。
  - 归档逐条校验，拒绝绝对路径 / .. / 符号链接(防 zip-slip)。
  - 解压/删除在线程池里做，不阻塞事件循环；临时目录用 . 开头，scan 不会误列。
"""
import os
import stat
import shutil
import tarfile
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Dict, List

from fastapi import APIRouter, File, Form, HTTPException, Response, UploadFile
from starlette.concurrency import run_in_threadpool

from services import process_manager as pm
from services import runtime_state
from services.app_scanner import scan_apps
from services.runtime_loader import load_runtime

package_install = load_runtime("package_install")

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
BINARY_NAME = os.environ.get("BINARY_NAME", "vision_analysis")

router = APIRouter()


@router.get("/apps", response_model=List[Dict[str, Any]])
async def list_apps(response: Response):
    response.headers["Cache-Control"] = "no-store, no-cache, must-revalidate"
    response.headers["Pragma"] = "no-cache"
    return await run_in_threadpool(scan_apps)


# ── 上传 / 删除程序包 ──────────────────────────────────────────────────────────

def _safe_name(name: str) -> str:
    name = (name or "").strip().strip("/")
    if not name or "/" in name or "\\" in name or ".." in name:
        raise HTTPException(status_code=400, detail="非法的程序名")
    if name.startswith((".", "_")):
        raise HTTPException(status_code=400, detail="程序名不能以 . 或 _ 开头（保留给控制台/隐藏目录）")
    return name


def _check_member(member: str) -> None:
    parts = Path(member).parts
    if member.startswith("/") or ".." in parts:
        raise ValueError(f"归档含非法路径: {member}")


def _extract(archive: Path, staging: Path) -> None:
    # 按内容(magic bytes)判格式，不看文件名——上传体落盘后叫 .tmp，靠扩展名会全部误判。
    with open(archive, "rb") as f:
        head = f.read(512)

    if head[:2] == b"\x1f\x8b":                                  # gzip → tar.gz
        kind = "targz"
    elif head[:2] == b"PK":                                      # zip (PK\x03\x04 等)
        kind = "zip"
    elif len(head) >= 262 and head[257:262] == b"ustar":         # 未压缩 tar
        kind = "tar"
    else:
        raise ValueError("无法识别的归档格式（仅支持 zip / tar.gz / tgz / tar）")

    if kind == "zip":
        with zipfile.ZipFile(archive) as z:
            for m in z.infolist():
                _check_member(m.filename)
                if stat.S_ISLNK(m.external_attr >> 16):
                    raise ValueError("归档包含链接文件，已拒绝")
            z.extractall(staging)
    else:
        mode = "r:gz" if kind == "targz" else "r:"
        with tarfile.open(archive, mode) as t:
            for m in t.getmembers():
                _check_member(m.name)
                if not (m.isfile() or m.isdir()):
                    raise ValueError("归档包含链接或特殊文件，已拒绝")
            t.extractall(staging)


def _resolve_root(staging: Path) -> Path:
    """归档若是单一顶层目录(如 dist/)，下沉到该目录；否则就用 staging 自身。"""
    entries = [p for p in staging.iterdir() if p.name != "__MACOSX"]
    if len(entries) == 1 and entries[0].is_dir():
        return entries[0]
    return staging


def _install_pkg(tmp_archive: Path, name: str) -> Dict[str, Any]:
    name = _safe_name(name)
    staging = Path(tempfile.mkdtemp(prefix=f".staging_{name}_", dir=str(APPS_ROOT)))
    try:
        _extract(tmp_archive, staging)
        return package_install.install(
            _resolve_root(staging), name, apps_root=APPS_ROOT,
            binary_name=BINARY_NAME, consume_source=True,
        )
    finally:
        # The runtime installer owns any rollback backup in its own staging dir.
        shutil.rmtree(staging, ignore_errors=True)
        tmp_archive.unlink(missing_ok=True)


@router.post("/apps/upload")
async def upload_app(file: UploadFile = File(...), name: str = Form("")):
    # 程序名：优先用表单 name，否则取归档文件名去扩展
    raw = name or Path(file.filename or "").name
    for ext in (".tar.gz", ".tgz", ".tar", ".zip"):
        if raw.lower().endswith(ext):
            raw = raw[: -len(ext)]
            break
    app_name = _safe_name(raw)

    APPS_ROOT.mkdir(parents=True, exist_ok=True)
    def _save() -> Path:
        file.file.seek(0)                       # 多段解析后指针可能在末尾，先回到开头
        tmp = None
        try:
            with tempfile.NamedTemporaryFile(dir=APPS_ROOT, prefix=f'.upload_{app_name}_', suffix='.tmp', delete=False) as out:
                tmp = Path(out.name)
                shutil.copyfileobj(file.file, out)
            return tmp
        except BaseException:
            if tmp is not None:
                tmp.unlink(missing_ok=True)
            raise

    tmp = None
    try:
        tmp = await run_in_threadpool(_save)
        info = await run_in_threadpool(_install_pkg, tmp, app_name)
    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=400, detail=f"安装失败: {e}")
    finally:
        if tmp is not None:
            tmp.unlink(missing_ok=True)
    return {"ok": True, **info}


@router.delete("/apps/{name}")
async def delete_app(name: str, delete_data: bool = False):
    app_name = _safe_name(name)
    def _delete() -> bool:
        # Keep inspection, stop, deletion and intent cleanup in one transaction.
        # stop_app re-enters the same thread's runtime lock without releasing it.
        with pm.runtime_lock():
            dest = APPS_ROOT / app_name
            if dest.is_symlink():
                raise HTTPException(status_code=400, detail="程序目录不能是符号链接")
            if not dest.is_dir():
                raise HTTPException(status_code=404, detail=f"程序不存在: {app_name}")
            pm.stop_app(app_name)
            shutil.rmtree(dest)
            data_deleted = False
            if delete_data:
                # Never follow an external link when removing app-owned data.
                app_data = APPS_ROOT / ".data" / app_name
                if app_data.is_symlink():
                    app_data.unlink()
                    data_deleted = True
                elif app_data.exists():
                    shutil.rmtree(app_data)
                    data_deleted = True
            runtime_state.remove_vision_app(app_name)
            return data_deleted

    data_deleted = await run_in_threadpool(_delete)
    return {"ok": True, "data_deleted": data_deleted}
