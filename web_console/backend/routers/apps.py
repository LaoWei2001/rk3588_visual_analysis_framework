"""
apps.py — 程序(App)列表 + 网页上传/删除程序包。

上传/删除做的就是 install_app.sh 的网页版：把一个打包好的程序(build.sh 产出的 dist：
二进制 + assets/ + libs/ + services/ ...)解压进 /opt/ai_apps/<name>/，或反向删除。
命令行 install_app.sh 仍可用，两者写的是同一处，互不冲突。

安全：
  - 程序名禁止 / .. 及 . / _ 开头(后者保留给 _console / 隐藏目录)。
  - 归档逐条校验，拒绝绝对路径 / .. / 符号链接(防 zip-slip)。
  - 解压/删除在线程池里做，不阻塞事件循环；临时目录用 . 开头，scan 不会误列。
"""
import os
import shutil
import tarfile
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Dict, List

from fastapi import APIRouter, File, Form, HTTPException, UploadFile
from starlette.concurrency import run_in_threadpool

from services import process_manager as pm
from services.app_scanner import scan_apps

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))
BINARY_NAME = os.environ.get("BINARY_NAME", "rk3588_yolo")

router = APIRouter()


@router.get("/apps", response_model=List[Dict[str, Any]])
async def list_apps():
    return scan_apps()


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
            for m in z.namelist():
                _check_member(m)
            z.extractall(staging)
    else:
        mode = "r:gz" if kind == "targz" else "r:"
        with tarfile.open(archive, mode) as t:
            for m in t.getmembers():
                _check_member(m.name)
                if m.issym() or m.islnk():
                    raise ValueError("归档包含链接文件，已拒绝")
            t.extractall(staging)


def _resolve_root(staging: Path) -> Path:
    """归档若是单一顶层目录(如 dist/)，下沉到该目录；否则就用 staging 自身。"""
    entries = [p for p in staging.iterdir() if p.name != "__MACOSX"]
    if len(entries) == 1 and entries[0].is_dir():
        return entries[0]
    return staging


def _install_pkg(tmp_archive: Path, name: str) -> Dict[str, Any]:
    dest = APPS_ROOT / name
    staging = Path(tempfile.mkdtemp(prefix=f".staging_{name}_", dir=str(APPS_ROOT)))
    dest_new = APPS_ROOT / f".{name}.new"
    try:
        _extract(tmp_archive, staging)
        root = _resolve_root(staging)

        # 二进制补可执行权限(zip 不保留 Unix 权限)
        binp = root / BINARY_NAME
        if binp.exists():
            binp.chmod(0o755)

        # 先就位到 .new，再 swap，缩短 dest 缺失窗口
        if dest_new.exists():
            shutil.rmtree(dest_new, ignore_errors=True)
        shutil.move(str(root), str(dest_new))

        pm.stop_app(name)                       # 同名在跑先停
        if dest.exists():
            shutil.rmtree(dest)
        shutil.move(str(dest_new), str(dest))

        return {
            "name": name,
            "has_binary": (dest / BINARY_NAME).exists(),
            "has_config": (dest / "assets" / "config.json").exists(),
        }
    finally:
        shutil.rmtree(staging, ignore_errors=True)
        if dest_new.exists():
            shutil.rmtree(dest_new, ignore_errors=True)
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
    tmp = APPS_ROOT / f".upload_{app_name}.tmp"

    def _save() -> None:
        file.file.seek(0)                       # 多段解析后指针可能在末尾，先回到开头
        with open(tmp, "wb") as out:
            shutil.copyfileobj(file.file, out)

    await run_in_threadpool(_save)

    try:
        info = await run_in_threadpool(_install_pkg, tmp, app_name)
    except HTTPException:
        raise
    except Exception as e:
        tmp.unlink(missing_ok=True)
        raise HTTPException(status_code=400, detail=f"安装失败: {e}")
    return {"ok": True, **info}


@router.delete("/apps/{name}")
async def delete_app(name: str):
    app_name = _safe_name(name)
    dest = APPS_ROOT / app_name
    if not dest.is_dir():
        raise HTTPException(status_code=404, detail=f"程序不存在: {app_name}")
    pm.stop_app(app_name)                       # 删除前先停进程
    await run_in_threadpool(lambda: shutil.rmtree(dest, ignore_errors=True))
    return {"ok": True}
