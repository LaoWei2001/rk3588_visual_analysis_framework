"""盒子网络设置 API。

路由只负责参数契约和 HTTP 错误转换；NetworkManager 操作、事务和回滚全部由
``services.network_manager`` 统一实现，其他板端管理入口也可复用同一服务。
"""
from __future__ import annotations

import asyncio
import grp
import os
import pwd
import re
from typing import List, Literal, Optional

from fastapi import APIRouter, BackgroundTasks, Depends, HTTPException, Query, Request
from pydantic import BaseModel, Field

from services import network_manager


router = APIRouter()


def require_network_admin(request: Request) -> None:
    """禁止普通 Linux 账号借助以 root 运行的控制台提升网络管理权限。"""
    session = getattr(request.state, "session", {})
    username = str(session.get("username", ""))
    try:
        account = pwd.getpwnam(username)
        group_ids = os.getgrouplist(username, account.pw_gid)
        group_names = {grp.getgrgid(group_id).gr_name for group_id in group_ids}
    except (KeyError, OSError):
        raise HTTPException(403, "无法确认当前账号的系统管理权限")
    if account.pw_uid != 0 and not group_names.intersection({"sudo", "wheel"}):
        raise HTTPException(403, "网络配置仅允许 root、sudo 或 wheel 管理员账号操作")


def _http_error(exc: Exception, status_code: int = 400) -> HTTPException:
    if isinstance(exc, FileNotFoundError):
        return HTTPException(501, f"当前系统缺少网络管理命令：{exc}")
    if isinstance(exc, network_manager.NetworkManagerError):
        return HTTPException(status_code, str(exc))
    return HTTPException(500, f"网络管理操作失败：{exc}")


class HostnameRequest(BaseModel):
    hostname: str = Field(min_length=1, max_length=253)


def _valid_hostname(value: str) -> bool:
    if len(value) > 253 or value.endswith("."):
        return False
    labels = value.split(".")
    return all(
        label and len(label) <= 63 and
        re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?", label)
        for label in labels
    )


class NetworkApplyRequest(BaseModel):
    device: str
    type: Literal["ethernet", "wifi"]
    connection_uuid: Optional[str] = None
    profile_name: str = Field(default="", max_length=80)
    method: Literal["auto", "manual"] = "auto"
    address: str = ""
    gateway: str = ""
    dns: List[str] = Field(default_factory=list, max_length=4)
    ssid: str = Field(default="", max_length=32)
    wifi_security: Literal["wpa-psk", "sae", "open"] = "wpa-psk"
    wifi_password: str = Field(default="", max_length=64)
    rollback_seconds: int = Field(default=60, ge=60, le=300)


class SavedConnectionRequest(BaseModel):
    connection_uuid: str
    device: str
    rollback_seconds: int = Field(default=60, ge=60, le=300)


class PingRequest(BaseModel):
    target: str


@router.get("/system/network")
async def get_network_settings():
    return await asyncio.to_thread(network_manager.network_snapshot)


@router.put("/system/network/hostname")
async def set_hostname(req: HostnameRequest, _admin: None = Depends(require_network_admin)):
    hostname = req.hostname.strip().lower()
    if not _valid_hostname(hostname):
        raise HTTPException(400, "设备名只能包含字母、数字、连字符和点，且连字符不能位于段首尾")
    try:
        command = [
            network_manager.command_path("hostnamectl", "HOSTNAMECTL_PATH"),
            "set-hostname", hostname,
        ]
        result = await asyncio.to_thread(network_manager.run_command, command)
    except Exception as exc:
        raise _http_error(exc, 500)
    if result.returncode != 0:
        raise HTTPException(500, (result.stderr or result.stdout or "设置设备名失败").strip())
    return {"ok": True, "hostname": hostname}


@router.post("/system/network/wifi/scan")
async def scan_wifi(device: str = Query(min_length=1, max_length=32), _admin: None = Depends(require_network_admin)):
    try:
        networks = await asyncio.to_thread(network_manager.scan_wifi, device)
        return {"device": device, "networks": networks}
    except Exception as exc:
        raise _http_error(exc)


@router.post("/system/network/changes")
async def start_network_change(req: NetworkApplyRequest, background_tasks: BackgroundTasks,
                               _admin: None = Depends(require_network_admin)):
    try:
        transaction = await asyncio.to_thread(network_manager.start_network_change, req.model_dump())
    except Exception as exc:
        raise _http_error(exc)
    # 先把事务编号和回滚期限返回浏览器，再切换网络，避免响应被换 IP 中断。
    background_tasks.add_task(network_manager.activate_transaction, transaction["id"])
    return {"ok": True, "transaction": transaction}


@router.post("/system/network/connections/activate")
async def activate_saved_connection(req: SavedConnectionRequest, background_tasks: BackgroundTasks,
                                    _admin: None = Depends(require_network_admin)):
    try:
        transaction = await asyncio.to_thread(
            network_manager.start_saved_connection,
            req.connection_uuid, req.device, req.rollback_seconds,
        )
    except Exception as exc:
        raise _http_error(exc)
    background_tasks.add_task(network_manager.activate_transaction, transaction["id"])
    return {"ok": True, "transaction": transaction}


@router.get("/system/network/transactions/{transaction_id}")
async def get_network_transaction(transaction_id: str):
    try:
        return await asyncio.to_thread(network_manager.transaction_status, transaction_id)
    except Exception as exc:
        raise _http_error(exc, 404)


@router.post("/system/network/transactions/{transaction_id}/confirm")
async def confirm_network_transaction(transaction_id: str, _admin: None = Depends(require_network_admin)):
    try:
        return await asyncio.to_thread(network_manager.confirm_transaction, transaction_id)
    except Exception as exc:
        raise _http_error(exc, 409)


@router.post("/system/network/transactions/{transaction_id}/rollback")
async def rollback_network_transaction(transaction_id: str, _admin: None = Depends(require_network_admin)):
    try:
        return await asyncio.to_thread(network_manager.rollback_transaction, transaction_id)
    except Exception as exc:
        raise _http_error(exc, 409)


@router.delete("/system/network/connections/{connection_uuid}")
async def delete_network_connection(connection_uuid: str, _admin: None = Depends(require_network_admin)):
    try:
        await asyncio.to_thread(network_manager.delete_connection, connection_uuid)
        return {"ok": True}
    except Exception as exc:
        raise _http_error(exc)


@router.post("/system/network/ping")
async def ping_network_target(req: PingRequest):
    try:
        return await asyncio.to_thread(network_manager.ping_target, req.target)
    except Exception as exc:
        raise _http_error(exc)
