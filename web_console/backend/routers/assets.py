import os
import shutil
from pathlib import Path

from fastapi import APIRouter, File, Form, HTTPException, UploadFile
from fastapi.concurrency import run_in_threadpool

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

router = APIRouter()

# 资源文件类型 → 类别（与 list_assets 的归类保持一致）
_MODEL_EXT = {".rknn"}
_LABEL_EXT = {".txt"}
_VIDEO_EXT = {".mp4", ".avi", ".mkv"}


def _category(suffix: str) -> str:
    s = suffix.lower()
    if s in _MODEL_EXT:
        return "model"
    if s in _LABEL_EXT:
        return "label"
    if s in _VIDEO_EXT:
        return "video"
    return ""


@router.get("/apps/{name}/assets")
async def list_assets(name: str):
    app_dir = APPS_ROOT / name
    if not app_dir.exists():
        raise HTTPException(status_code=404, detail=f"App '{name}' not found")

    assets_dir = app_dir / "assets"
    models, labels, videos = [], [], []

    if assets_dir.exists():
        for f in sorted(assets_dir.iterdir()):
            if not f.is_file():
                continue
            rel = f"assets/{f.name}"
            if f.suffix == ".rknn":
                models.append(rel)
            elif f.suffix == ".txt":
                labels.append(rel)
            elif f.suffix in (".mp4", ".avi", ".mkv"):
                videos.append(rel)

    return {"models": models, "labels": labels, "videos": videos}


@router.post("/apps/{name}/assets/upload")
async def upload_asset(
    name: str,
    file: UploadFile = File(...),
    overwrite: str = Form("false"),
):
    """导入一个视频 / 模型 / 标签文件到 assets/。

    - 仅接受 .rknn / .txt / .mp4 / .avi / .mkv。
    - 若同名文件已存在：overwrite=true 覆盖；否则另存为 <名>_copy.<扩展>（保留原文件）。
    返回最终落盘的相对路径，便于前端自动选中。
    """
    app_dir = APPS_ROOT / name
    if not app_dir.exists():
        raise HTTPException(status_code=404, detail=f"App '{name}' not found")

    fname = Path(file.filename or "").name        # 去掉任何目录部分，防路径穿越
    if not fname:
        raise HTTPException(status_code=400, detail="文件名为空")
    cat = _category(Path(fname).suffix)
    if not cat:
        raise HTTPException(
            status_code=400,
            detail=f"不支持的文件类型: {Path(fname).suffix or '无扩展名'}（仅 .rknn/.txt/.mp4/.avi/.mkv）",
        )

    assets_dir = app_dir / "assets"
    assets_dir.mkdir(exist_ok=True)

    do_overwrite = str(overwrite).lower() in ("1", "true", "yes", "on")
    target  = assets_dir / fname
    renamed = False
    if target.exists() and not do_overwrite:
        stem, ext = Path(fname).stem, Path(fname).suffix
        stem = f"{stem}_copy"
        cand = assets_dir / f"{stem}{ext}"
        while cand.exists():                       # _copy 仍冲突 → 继续追加 _copy
            stem = f"{stem}_copy"
            cand = assets_dir / f"{stem}{ext}"
        target  = cand
        renamed = True

    tmp = assets_dir / f".upload_{target.name}.tmp"

    def _save() -> None:
        file.file.seek(0)                          # 多段解析后指针可能在末尾
        with open(tmp, "wb") as out:
            shutil.copyfileobj(file.file, out)
        os.replace(tmp, target)                    # 同盘原子替换

    try:
        await run_in_threadpool(_save)
    except Exception as e:
        try:
            tmp.unlink(missing_ok=True)
        except OSError:
            pass
        raise HTTPException(status_code=500, detail=f"保存失败: {e}")

    return {
        "ok": True,
        "path": f"assets/{target.name}",
        "name": target.name,
        "category": cat,
        "renamed": renamed,
    }
