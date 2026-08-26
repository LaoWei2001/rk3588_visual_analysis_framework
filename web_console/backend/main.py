import asyncio
import os
import random
import re
from contextlib import asynccontextmanager
from contextlib import suppress
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

from routers import (apps, assets, auth, camera_settings, logic_control, config_io, logs, network_settings,
                     ota_config, process, records, services, snapshot, storage_settings, stream,
                     system_settings, terminal, delivery_config, video_capture)
from services.auth_service import get_session
from services import process_manager as process_manager
from services import runtime_state
from services import network_manager
from services import camera_manager
from services.camera_web_proxy import camera_web_proxy
from services.video_capture_manager import capture_manager

FRONTEND_DIST = Path(__file__).parent.parent / "frontend" / "dist"

# 侧边栏 / 登录页 logo: 用户把图片或 GIF 放进 frontend/logos/, 每次打开网页随机取一张;
# 目录为空 / 不存在时回退到 frontend/logo.png。下面是允许的扩展名 → MIME 类型。
LOGO_DIR      = Path(__file__).parent.parent / "frontend" / "logos"
LOGO_FALLBACK = Path(__file__).parent.parent / "frontend" / "logo.png"
_LOGO_MIME = {
    ".png": "image/png",  ".apng": "image/apng",
    ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
    ".gif": "image/gif",  ".webp": "image/webp",
    ".bmp": "image/bmp",  ".svg":  "image/svg+xml",
}

# Paths that are always public (no auth required)
_PUBLIC_API = {"/api/auth/login"}
_PUBLIC_LOGIC_ACTION = re.compile(
    r"^/api/apps/[^/]+/(?:channels/\d+|global-logics/[^/]+)/actions/[^/]+$"
)


@asynccontextmanager
async def lifespan(app: FastAPI):
    # 网络事务由独立 systemd 定时器保护。控制台异常重启时先确认保护仍存在，
    # 若原切换工作中途退出则立即恢复原连接并清理临时配置。
    try:
        await asyncio.to_thread(network_manager.recover_incomplete_transactions)
    except Exception as exc:
        print(f"[Network] 未完成网络事务恢复失败：{exc}")
    # 摄像头侧仅恢复独立 /32 地址和主机路由，不改默认路由或 NetworkManager 配置。
    try:
        await asyncio.to_thread(camera_manager.restore_persisted_configuration)
    except Exception as exc:
        print(f"[CameraNetwork] 持久化配置恢复失败：{exc}")
    process_manager.recover_processes()

    def restore_runtime() -> None:
        target = runtime_state.get_vision_boot_target()
        if target is None:
            print("[Autostart] 没有满足条件的视觉程序，跳过运行组合恢复")
            return
        try:
            current = process_manager.get_running_app_context()
            if current is not None and current["app"] != target["app"]:
                print(
                    f"[Autostart] 已有视觉程序 {current['app']} 运行，"
                    f"跳过目标 {target['app']}"
                )
                return
            if current is None:
                process_manager.start_app(target["app"], target["mode"], target["config"])
                print(
                    f"[Autostart] 已恢复视觉程序 {target['app']} "
                    f"({target['mode']}, {target['config']})"
                )
            result = services.sync_services_for_running_app()
            for message in result["errors"]:
                print(f"[Autostart] 后台服务恢复失败：{message}")
            if result["updated"]:
                print(f"[Autostart] 已恢复/重新绑定后台服务：{', '.join(result['updated'])}")
        except Exception as exc:
            print(f"[Autostart] 运行组合恢复失败：{exc}")

    # systemctl/Popen 都是阻塞调用，放入工作线程，避免卡住事件循环。
    await asyncio.to_thread(restore_runtime)
    storage_task = asyncio.create_task(storage_settings.maintenance_loop())
    try:
        yield
    finally:
        # 摄像头 Web 代理仅按需启动；退出时关闭监听和所有透明转发连接。
        await asyncio.to_thread(camera_web_proxy.stop)
        # 独立素材采集器不依赖浏览器连接，但控制台正常退出时必须发送 EOS，
        # 确保正在写入的单个 MP4 可以正常收尾。
        await asyncio.to_thread(capture_manager.shutdown)
        storage_task.cancel()
        with suppress(asyncio.CancelledError):
            await storage_task


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
    public_logic_action = (
        request.method == "POST"
        and _PUBLIC_LOGIC_ACTION.fullmatch(path) is not None
    )

    # Static files, SPA HTML, health check → always pass through
    if (
        request.method == "OPTIONS"
        or not path.startswith("/api/")
        or path in _PUBLIC_API
        or path == "/health"
        or public_logic_action
    ):
        return await call_next(request)

    # 优先取 Authorization 头；无法自定义请求头的原生 <img>/<video> 资源使用 ?token=。
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

    session = get_session(token)
    if not session:
        return JSONResponse(
            status_code=401,
            content={"detail": "登录已过期，请重新登录"},
        )

    request.state.session = session
    return await call_next(request)


# ── API & WebSocket routes ─────────────────────────────────────────────────
app.include_router(auth.router,      prefix="/api")
app.include_router(apps.router,      prefix="/api")
app.include_router(config_io.router, prefix="/api")
app.include_router(logic_control.router, prefix="/api")
app.include_router(assets.router,    prefix="/api")
app.include_router(process.router,   prefix="/api")
app.include_router(snapshot.router,  prefix="/api")
app.include_router(records.router,   prefix="/api")
app.include_router(stream.router,    prefix="/api")
app.include_router(delivery_config.router, prefix="/api")
app.include_router(ota_config.router,    prefix="/api")
app.include_router(services.router,      prefix="/api")
app.include_router(storage_settings.router, prefix="/api")
app.include_router(network_settings.router, prefix="/api")
app.include_router(camera_settings.router, prefix="/api")
app.include_router(system_settings.router, prefix="/api")
app.include_router(video_capture.router, prefix="/api")
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


@app.get("/logo/random")
async def serve_random_logo():
    """从 frontend/logos/ 随机返回一张图片或 GIF。每次请求都重新随机, 故每次打开网页
    (前端用 ?t=<nonce> 触发) 都可能是不同的一张。目录为空/不存在则回退 logo.png。
    公开(无需登录), 登录页也用它。GIF 由浏览器 <img> 原生播放。"""
    pick = None
    try:
        if LOGO_DIR.is_dir():
            pool = [f for f in LOGO_DIR.iterdir()
                    if f.is_file() and f.suffix.lower() in _LOGO_MIME]
            if pool:
                pick = random.choice(pool)
    except OSError:
        pick = None
    if pick is None:
        pick = LOGO_FALLBACK
    if not pick.exists():
        raise HTTPException(status_code=404, detail="no logo available")
    media = _LOGO_MIME.get(pick.suffix.lower(), "image/png")
    return FileResponse(str(pick), media_type=media,
                        headers={"Cache-Control": "no-cache, no-store, must-revalidate"})


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
