"""盒子网络状态与 NetworkManager IPv4 配置。"""
from __future__ import annotations

import ipaddress
import os
import re
import shutil
import socket
import subprocess
from pathlib import Path
from typing import Any, Dict, List

from fastapi import APIRouter, BackgroundTasks, HTTPException
from pydantic import BaseModel, Field


router = APIRouter()
_UUID_RE = re.compile(r"^[0-9A-Fa-f-]{32,36}$")


def _command_path(name: str, override_env: str) -> str:
    override = os.environ.get(override_env, "").strip()
    if override:
        path = Path(override)
        if not path.is_absolute():
            raise RuntimeError(f"{override_env} 必须是绝对路径")
        return str(path)
    found = shutil.which(name)
    if not found:
        raise FileNotFoundError(name)
    return str(Path(found).resolve())


def _run(cmd: List[str], timeout: int = 20) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env["LC_ALL"] = "C"
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)


def _nmcli(*args: str, timeout: int = 20) -> subprocess.CompletedProcess:
    return _run([_command_path("nmcli", "NMCLI_PATH"), *args], timeout=timeout)


def _unescape(value: str) -> str:
    output: List[str] = []
    escaped = False
    for char in value:
        if escaped:
            output.append(char)
            escaped = False
        elif char == "\\":
            escaped = True
        else:
            output.append(char)
    if escaped:
        output.append("\\")
    return "".join(output)


def _split_terse(line: str) -> List[str]:
    fields: List[str] = []
    current: List[str] = []
    escaped = False
    for char in line:
        if escaped:
            current.append(char)
            escaped = False
        elif char == "\\":
            escaped = True
        elif char == ":":
            fields.append("".join(current))
            current = []
        else:
            current.append(char)
    fields.append("".join(current))
    return fields


def _values(field: str, device: str) -> List[str]:
    result = _nmcli("-g", field, "device", "show", device)
    if result.returncode != 0:
        return []
    return [_unescape(line.strip()) for line in result.stdout.splitlines() if line.strip()]


def _connection_values(field: str, uuid: str) -> List[str]:
    result = _nmcli("-g", field, "connection", "show", "uuid", uuid)
    if result.returncode != 0:
        return []
    return [_unescape(line.strip()) for line in result.stdout.splitlines() if line.strip()]


def network_snapshot() -> Dict[str, Any]:
    try:
        nmcli_path = _command_path("nmcli", "NMCLI_PATH")
    except (FileNotFoundError, RuntimeError) as exc:
        return {
            "hostname": socket.gethostname(),
            "manager": "unavailable",
            "config_supported": False,
            "interfaces": [],
            "error": f"未找到 NetworkManager/nmcli：{exc}",
        }

    status = _run([
        nmcli_path, "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status",
    ])
    if status.returncode != 0:
        return {
            "hostname": socket.gethostname(),
            "manager": "NetworkManager",
            "config_supported": False,
            "interfaces": [],
            "error": (status.stderr or status.stdout or "nmcli 查询失败").strip(),
        }

    interfaces: List[Dict[str, Any]] = []
    for line in status.stdout.splitlines():
        fields = _split_terse(line)
        if len(fields) < 4:
            continue
        device, kind, state, connection = fields[:4]
        if kind not in ("ethernet", "wifi") or not device:
            continue
        uuid_values = _values("GENERAL.CON-UUID", device)
        uuid = uuid_values[0] if uuid_values and _UUID_RE.fullmatch(uuid_values[0]) else ""
        method_values = _connection_values("ipv4.method", uuid) if uuid else []
        interfaces.append({
            "device": device,
            "type": kind,
            "state": state,
            "connection": connection,
            "connection_uuid": uuid or None,
            "mac": (_values("GENERAL.HWADDR", device) or [""])[0],
            "addresses": _values("IP4.ADDRESS", device),
            "gateway": (_values("IP4.GATEWAY", device) or [""])[0],
            "dns": _values("IP4.DNS", device),
            "ipv4_method": (method_values or [""])[0],
            "configurable": bool(uuid),
        })
    return {
        "hostname": socket.gethostname(),
        "manager": "NetworkManager",
        "config_supported": True,
        "interfaces": interfaces,
        "error": None,
    }


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


@router.get("/system/network")
async def get_network_settings():
    return network_snapshot()


@router.put("/system/network/hostname")
async def set_hostname(req: HostnameRequest):
    hostname = req.hostname.strip().lower()
    if not _valid_hostname(hostname):
        raise HTTPException(400, "设备名只能包含字母、数字、连字符和点，且连字符不能位于段首尾")
    try:
        command = [_command_path("hostnamectl", "HOSTNAMECTL_PATH"), "set-hostname", hostname]
        result = _run(command)
    except Exception as exc:
        raise HTTPException(500, f"设置设备名失败：{exc}")
    if result.returncode != 0:
        raise HTTPException(500, (result.stderr or result.stdout or "设置设备名失败").strip())
    return {"ok": True, "hostname": hostname}


class IPv4Request(BaseModel):
    connection_uuid: str
    method: str
    address: str = ""
    gateway: str = ""
    dns: List[str] = Field(default_factory=list, max_length=4)


def _activate_connection(uuid: str) -> None:
    try:
        result = _nmcli("connection", "up", "uuid", uuid, timeout=45)
        if result.returncode != 0:
            print(f"[Network] connection activation failed: {(result.stderr or result.stdout).strip()}")
    except Exception as exc:
        print(f"[Network] connection activation failed: {exc}")


@router.put("/system/network/ipv4")
async def set_ipv4(req: IPv4Request, background_tasks: BackgroundTasks):
    uuid = req.connection_uuid.strip()
    if not _UUID_RE.fullmatch(uuid):
        raise HTTPException(400, "非法的 NetworkManager 连接 UUID")
    if req.method not in ("auto", "manual"):
        raise HTTPException(400, "IPv4 模式只支持 DHCP(auto) 或静态(manual)")

    if req.method == "manual":
        try:
            interface = ipaddress.IPv4Interface(req.address.strip())
            gateway_value = ipaddress.IPv4Address(req.gateway.strip())
            if gateway_value not in interface.network:
                raise ValueError(f"网关 {gateway_value} 不在地址 {interface} 的网段内")
            address = str(interface)
            gateway = str(gateway_value)
            dns = [str(ipaddress.IPv4Address(value.strip())) for value in req.dns if value.strip()]
        except ValueError as exc:
            raise HTTPException(400, f"静态 IPv4 参数无效：{exc}")
        args = [
            "connection", "modify", "uuid", uuid,
            "ipv4.method", "manual",
            "ipv4.addresses", address,
            "ipv4.gateway", gateway,
            "ipv4.dns", ",".join(dns),
        ]
    else:
        args = [
            "connection", "modify", "uuid", uuid,
            "ipv4.method", "auto",
            "ipv4.addresses", "",
            "ipv4.gateway", "",
            "ipv4.dns", "",
            "ipv4.ignore-auto-dns", "no",
        ]

    try:
        changed = _nmcli(*args)
    except FileNotFoundError:
        raise HTTPException(501, "当前盒子没有 NetworkManager/nmcli，不能修改网络")
    except Exception as exc:
        raise HTTPException(500, f"修改网络失败：{exc}")
    if changed.returncode != 0:
        raise HTTPException(500, (changed.stderr or changed.stdout or "修改网络失败").strip())

    # 响应发送完成后再激活连接，尽量让浏览器先收到“即将断线”的结果。
    background_tasks.add_task(_activate_connection, uuid)
    return {"ok": True, "activation_scheduled": True}
