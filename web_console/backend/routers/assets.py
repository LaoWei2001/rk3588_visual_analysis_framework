import os
from pathlib import Path

from fastapi import APIRouter, HTTPException

APPS_ROOT = Path(os.environ.get("APPS_ROOT", "/opt/ai_apps"))

router = APIRouter()


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
