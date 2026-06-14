import os
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from routers import (apps, assets, auth, config_io, logs, ota_config, process,
                     records, services, snapshot, stream, terminal, upload_config)
from services.auth_service import get_session
from services.process_manager import recover_processes

FRONTEND_DIST = Path(__file__).parent.parent / "frontend" / "dist"

# Paths that are always public (no auth required)
_PUBLIC_API = {"/api/auth/login"}


@asynccontextmanager
async def lifespan(app: FastAPI):
    recover_processes()
    yield


app = FastAPI(title="RK3588 Web Console", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.middleware("http")
async def auth_middleware(request: Request, call_next):
    """Protect all /api/* endpoints except the public ones."""
    path = request.url.path

    # Static files, SPA HTML, health check → always pass through
    if not path.startswith("/api/") or path in _PUBLIC_API or path == "/health":
        return await call_next(request)

    # 优先取 Authorization 头; 对于无法带头的 <img>/<video> 流(如 /stream),
    # 退而取 ?token= 查询参数。
    auth_header = request.headers.get("Authorization", "")
    if auth_header.startswith("Bearer "):
        token = auth_header[7:]
    else:
        token = request.query_params.get("token", "")

    if not token:
        return JSONResponse(
            status_code=401,
            content={"detail": "未登录，请先使用 SSH 账号密码登录"},
        )

    if not get_session(token):
        return JSONResponse(
            status_code=401,
            content={"detail": "登录已过期，请重新登录"},
        )

    return await call_next(request)


# ── API & WebSocket routes ─────────────────────────────────────────────────
app.include_router(auth.router,      prefix="/api")
app.include_router(apps.router,      prefix="/api")
app.include_router(config_io.router, prefix="/api")
app.include_router(assets.router,    prefix="/api")
app.include_router(process.router,   prefix="/api")
app.include_router(snapshot.router,  prefix="/api")
app.include_router(records.router,   prefix="/api")
app.include_router(stream.router,    prefix="/api")
app.include_router(upload_config.router, prefix="/api")
app.include_router(ota_config.router,    prefix="/api")
app.include_router(services.router,      prefix="/api")
app.include_router(logs.router)      # WebSocket has its own /ws prefix
app.include_router(terminal.router)  # WebSocket terminal


@app.get("/health")
async def health():
    return {"status": "ok"}


# ── Serve user-replaceable static images (public, no auth) ────────────────
# Replace  web_console/frontend/logo.png  to update the login page logo.
# Replace  web_console/frontend/img.png   to update the display image.
# No rebuild needed — served directly from the source folder.

@app.get("/logo.png")
async def serve_logo():
    path = Path(__file__).parent.parent / "frontend" / "logo.png"
    if not path.exists():
        raise HTTPException(status_code=404, detail="logo.png not found in frontend/")
    return FileResponse(str(path), media_type="image/png",
                        headers={"Cache-Control": "no-cache, no-store"})


@app.get("/img.png")
async def serve_display_image():
    path = Path(__file__).parent.parent / "frontend" / "img.png"
    if not path.exists():
        raise HTTPException(status_code=404, detail="img.png not found in frontend/")
    return FileResponse(str(path), media_type="image/png",
                        headers={"Cache-Control": "no-cache, no-store"})


# ── Serve React SPA (must be last) ────────────────────────────────────────
if FRONTEND_DIST.exists():
    app.mount(
        "/assets",
        StaticFiles(directory=str(FRONTEND_DIST / "assets")),
        name="static-assets",
    )

    @app.get("/{full_path:path}")
    async def spa_fallback(full_path: str):
        target = FRONTEND_DIST / full_path
        if target.is_file():
            return FileResponse(str(target))
        return FileResponse(str(FRONTEND_DIST / "index.html"))
else:
    @app.get("/")
    async def root():
        return {"message": "Frontend not built. Run: cd frontend && npm run build"}
