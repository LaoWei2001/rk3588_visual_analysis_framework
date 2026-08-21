"""NetworkManager 的可复用网络管理服务。

所有会改变连通性的操作都使用临时连接和持久化回滚定时器。Web 控制台即使因
换 IP 失联或重启，systemd 启动的回滚任务仍会恢复原连接；只有显式确认后才会
把临时连接提升为正式连接。
"""
from __future__ import annotations

import fcntl
import ipaddress
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
import uuid as uuid_lib
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

try:
    from .data_dir import APPS_ROOT
except ImportError:  # 由 systemd 直接执行本文件时
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from services.data_dir import APPS_ROOT


TRANSACTION_DIR = Path(os.environ.get(
    "NETWORK_TRANSACTION_DIR", str(APPS_ROOT / ".network_transactions")
))
DEFAULT_ROLLBACK_SECONDS = 60
MIN_ROLLBACK_SECONDS = 60
MAX_ROLLBACK_SECONDS = 300
ACTIVATION_ROLLBACK_GRACE_SECONDS = 60
_UUID_RE = re.compile(r"^[0-9A-Fa-f-]{32,36}$")
_DEVICE_RE = re.compile(r"^[A-Za-z0-9_.:-]{1,32}$")
_PROFILE_NAME_RE = re.compile(r"^[^\x00-\x1f\x7f]{1,80}$")
_LOCAL_LOCK = threading.RLock()


class NetworkManagerError(RuntimeError):
    pass


def command_path(name: str, override_env: str) -> str:
    override = os.environ.get(override_env, "").strip()
    if override:
        path = Path(override)
        if not path.is_absolute():
            raise NetworkManagerError(f"{override_env} 必须是绝对路径")
        return str(path)
    found = shutil.which(name)
    if not found:
        raise FileNotFoundError(name)
    return str(Path(found).resolve())


def run_command(command: List[str], timeout: int = 20) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env["LC_ALL"] = "C"
    return subprocess.run(command, capture_output=True, text=True, timeout=timeout, env=env)


def nmcli(*args: str, timeout: int = 20) -> subprocess.CompletedProcess:
    return run_command([command_path("nmcli", "NMCLI_PATH"), *args], timeout=timeout)


def _checked_nmcli(*args: str, timeout: int = 20, action: str = "执行网络操作") -> str:
    result = nmcli(*args, timeout=timeout)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or f"{action}失败").strip()
        raise NetworkManagerError(detail)
    return result.stdout


def unescape_terse(value: str) -> str:
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


def split_terse(line: str) -> List[str]:
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


def _values(scope: str, field: str, identifier: str) -> List[str]:
    if scope == "device":
        result = nmcli("-g", field, "device", "show", identifier)
    else:
        result = nmcli("-g", field, "connection", "show", "uuid", identifier)
    if result.returncode != 0:
        return []
    return [unescape_terse(line.strip()) for line in result.stdout.splitlines() if line.strip()]


def _validate_uuid(value: str) -> str:
    clean = value.strip()
    if not _UUID_RE.fullmatch(clean):
        raise NetworkManagerError("非法的 NetworkManager 连接 UUID")
    return clean


def _validate_device(value: str) -> str:
    clean = value.strip()
    if not _DEVICE_RE.fullmatch(clean):
        raise NetworkManagerError("非法的网卡名称")
    return clean


def _validate_profile_name(value: str) -> str:
    clean = value.strip()
    if not _PROFILE_NAME_RE.fullmatch(clean):
        raise NetworkManagerError("连接名称不能为空、不能包含控制字符，且最多 80 个字符")
    return clean


def _connection_rows() -> List[Dict[str, Any]]:
    output = _checked_nmcli(
        "-t", "-f", "NAME,UUID,TYPE,DEVICE,AUTOCONNECT",
        "connection", "show", action="读取连接配置",
    )
    rows: List[Dict[str, Any]] = []
    for line in output.splitlines():
        fields = split_terse(line)
        if len(fields) < 5 or not _UUID_RE.fullmatch(fields[1]):
            continue
        kind = fields[2]
        if kind not in ("802-3-ethernet", "802-11-wireless", "ethernet", "wifi"):
            continue
        rows.append({
            "name": fields[0],
            "uuid": fields[1],
            "type": "wifi" if kind in ("802-11-wireless", "wifi") else "ethernet",
            "device": "" if fields[3] == "--" else fields[3],
            "autoconnect": fields[4].lower() == "yes",
            "active": fields[3] not in ("", "--"),
        })
    return rows


def _device_rows() -> List[Dict[str, str]]:
    output = _checked_nmcli(
        "-t", "-f", "DEVICE,TYPE,STATE,CONNECTION", "device", "status",
        action="读取网卡状态",
    )
    rows: List[Dict[str, str]] = []
    for line in output.splitlines():
        fields = split_terse(line)
        if len(fields) < 4 or fields[1] not in ("ethernet", "wifi") or not fields[0]:
            continue
        rows.append({
            "device": fields[0], "type": fields[1], "state": fields[2],
            "connection": "" if fields[3] == "--" else fields[3],
        })
    return rows


def _transaction_path(transaction_id: str) -> Path:
    if not re.fullmatch(r"[0-9a-f]{32}", transaction_id):
        raise NetworkManagerError("非法的网络事务编号")
    return TRANSACTION_DIR / f"{transaction_id}.json"


@contextmanager
def _transaction_lock():
    TRANSACTION_DIR.mkdir(parents=True, exist_ok=True)
    lock_path = TRANSACTION_DIR / ".lock"
    with _LOCAL_LOCK, lock_path.open("a+") as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def _write_transaction(state: Dict[str, Any]) -> None:
    TRANSACTION_DIR.mkdir(parents=True, exist_ok=True)
    path = _transaction_path(state["id"])
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def _read_transaction(transaction_id: str) -> Dict[str, Any]:
    path = _transaction_path(transaction_id)
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise NetworkManagerError("网络切换事务不存在或已经清理") from exc
    except (OSError, ValueError, TypeError) as exc:
        raise NetworkManagerError(f"网络切换事务损坏：{exc}") from exc
    if raw.get("id") != transaction_id:
        raise NetworkManagerError("网络切换事务内容不匹配")
    return raw


def _public_transaction(state: Dict[str, Any]) -> Dict[str, Any]:
    deadline = float(state.get("deadline", 0))
    return {
        "id": state["id"],
        "status": state.get("status", "unknown"),
        "device": state.get("device", ""),
        "kind": state.get("kind", ""),
        "old_uuid": state.get("old_uuid") or None,
        "new_uuid": state.get("new_uuid") or None,
        "profile_name": state.get("final_name", ""),
        "target_addresses": state.get("target_addresses", []),
        "deadline": deadline,
        "remaining_seconds": max(0, int(deadline - time.time())) if deadline else 0,
        "error": state.get("error") or state.get("failure_reason") or None,
    }


def _active_transaction() -> Optional[Dict[str, Any]]:
    if not TRANSACTION_DIR.exists():
        return None
    active_statuses = {"scheduled", "activating", "awaiting_confirmation", "committing", "rolling_back"}
    candidates: List[Dict[str, Any]] = []
    for path in TRANSACTION_DIR.glob("*.json"):
        try:
            state = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError, TypeError):
            continue
        if state.get("status") in active_statuses:
            candidates.append(state)
    return max(candidates, key=lambda item: float(item.get("created_at", 0))) if candidates else None


def network_snapshot() -> Dict[str, Any]:
    try:
        command_path("nmcli", "NMCLI_PATH")
        devices = _device_rows()
        connections = _connection_rows()
    except (FileNotFoundError, NetworkManagerError, subprocess.SubprocessError) as exc:
        return {
            "hostname": socket.gethostname(), "manager": "unavailable",
            "config_supported": False, "rollback_supported": False,
            "interfaces": [], "connections": [], "pending_transaction": None,
            "error": f"未找到可用的 NetworkManager/nmcli：{exc}",
        }

    by_uuid = {row["uuid"]: row for row in connections}
    interfaces: List[Dict[str, Any]] = []
    for row in devices:
        device = row["device"]
        uuid_values = _values("device", "GENERAL.CON-UUID", device)
        connection_uuid = uuid_values[0] if uuid_values and _UUID_RE.fullmatch(uuid_values[0]) else ""
        method_values = _values("connection", "ipv4.method", connection_uuid) if connection_uuid else []
        ssid_values = _values("connection", "802-11-wireless.ssid", connection_uuid) if connection_uuid and row["type"] == "wifi" else []
        interfaces.append({
            **row,
            "connection_uuid": connection_uuid or None,
            "mac": (_values("device", "GENERAL.HWADDR", device) or [""])[0],
            "addresses": _values("device", "IP4.ADDRESS", device),
            "gateway": (_values("device", "IP4.GATEWAY", device) or [""])[0],
            "dns": _values("device", "IP4.DNS", device),
            "ipv4_method": (method_values or [""])[0],
            "ssid": (ssid_values or [""])[0],
            "configurable": row["state"] != "unmanaged",
            "active_profile": by_uuid.get(connection_uuid),
        })

    for connection in connections:
        connection_uuid = connection["uuid"]
        connection["ipv4_method"] = (_values("connection", "ipv4.method", connection_uuid) or [""])[0]
        connection["addresses"] = _values("connection", "ipv4.addresses", connection_uuid)
        connection["gateway"] = (_values("connection", "ipv4.gateway", connection_uuid) or [""])[0]
        connection["dns"] = _values("connection", "ipv4.dns", connection_uuid)
        connection["ssid"] = (
            (_values("connection", "802-11-wireless.ssid", connection_uuid) or [""])[0]
            if connection["type"] == "wifi" else ""
        )
        connection["security"] = (
            (_values("connection", "802-11-wireless-security.key-mgmt", connection_uuid) or ["open"])[0]
            if connection["type"] == "wifi" else ""
        )

    rollback_supported = bool(shutil.which("systemd-run") and shutil.which("systemctl"))
    pending = _active_transaction()
    return {
        "hostname": socket.gethostname(), "manager": "NetworkManager",
        "config_supported": rollback_supported, "rollback_supported": rollback_supported,
        "interfaces": interfaces, "connections": connections,
        "pending_transaction": _public_transaction(pending) if pending else None,
        "error": None if rollback_supported else "缺少 systemd-run/systemctl，已禁止不带自动回滚的网络修改",
    }


def validate_ipv4(method: str, address: str, gateway: str, dns_values: Iterable[str]) -> Dict[str, Any]:
    if method not in ("auto", "manual"):
        raise NetworkManagerError("IPv4 模式只支持 DHCP(auto) 或静态(manual)")
    if method == "auto":
        return {"method": "auto", "address": "", "gateway": "", "dns": []}
    try:
        interface = ipaddress.IPv4Interface(address.strip())
        if interface.network.prefixlen < 31 and interface.ip in (
            interface.network.network_address, interface.network.broadcast_address,
        ):
            raise ValueError("不能使用网段地址或广播地址作为设备地址")
        gateway_value: Optional[ipaddress.IPv4Address] = None
        if gateway.strip():
            gateway_value = ipaddress.IPv4Address(gateway.strip())
            if gateway_value not in interface.network:
                raise ValueError(f"网关 {gateway_value} 不在地址 {interface} 的网段内")
        dns = [str(ipaddress.IPv4Address(value.strip())) for value in dns_values if value.strip()]
        if len(dns) > 4:
            raise ValueError("DNS 最多填写 4 个")
    except ValueError as exc:
        raise NetworkManagerError(f"静态 IPv4 参数无效：{exc}") from exc
    return {
        "method": "manual", "address": str(interface),
        "gateway": str(gateway_value) if gateway_value else "", "dns": dns,
    }


def _assert_device(device: str, expected_type: Optional[str] = None) -> Dict[str, str]:
    clean = _validate_device(device)
    row = next((item for item in _device_rows() if item["device"] == clean), None)
    if not row:
        raise NetworkManagerError(f"网卡 {clean} 不存在或未由 NetworkManager 管理")
    if expected_type and row["type"] != expected_type:
        raise NetworkManagerError(f"网卡 {clean} 不是{expected_type}类型")
    if row["state"] == "unmanaged":
        raise NetworkManagerError(f"网卡 {clean} 未由 NetworkManager 托管")
    return row


def scan_wifi(device: str) -> List[Dict[str, Any]]:
    clean = _assert_device(device, "wifi")["device"]
    _checked_nmcli("radio", "wifi", "on", action="开启 Wi-Fi")
    output = _checked_nmcli(
        "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY",
        "device", "wifi", "list", "ifname", clean,
        timeout=30, action="扫描 Wi-Fi",
    )
    networks: Dict[str, Dict[str, Any]] = {}
    for line in output.splitlines():
        fields = split_terse(line)
        if len(fields) < 4 or not fields[1]:
            continue
        try:
            signal = max(0, min(100, int(fields[2] or 0)))
        except ValueError:
            continue
        item = {
            "in_use": fields[0].strip() == "*", "ssid": fields[1],
            "signal": signal, "security": fields[3] or "OPEN",
        }
        previous = networks.get(fields[1])
        if previous is None or item["signal"] > previous["signal"]:
            networks[fields[1]] = item
    return sorted(networks.values(), key=lambda item: (not item["in_use"], -item["signal"], item["ssid"]))


def _profile_by_uuid(connection_uuid: str) -> Dict[str, Any]:
    clean = _validate_uuid(connection_uuid)
    row = next((item for item in _connection_rows() if item["uuid"] == clean), None)
    if not row:
        raise NetworkManagerError("指定的连接配置不存在")
    return row


def _unique_profile_name(preferred: str, ignored_uuid: str = "") -> str:
    base = _validate_profile_name(preferred)
    used = {item["name"] for item in _connection_rows() if item["uuid"] != ignored_uuid}
    if base not in used:
        return base
    for index in range(2, 1000):
        suffix = f" ({index})"
        candidate = base[:80 - len(suffix)] + suffix
        if candidate not in used:
            return candidate
    raise NetworkManagerError("无法生成唯一的连接名称")


def _configure_ipv4(connection_uuid: str, config: Dict[str, Any]) -> None:
    if config["method"] == "manual":
        _checked_nmcli(
            "connection", "modify", "uuid", connection_uuid,
            "ipv4.method", "manual", "ipv4.addresses", config["address"],
            "ipv4.gateway", config["gateway"], "ipv4.dns", ",".join(config["dns"]),
            "ipv4.ignore-auto-dns", "yes", "ipv4.dad-timeout", "3000",
            action="设置静态 IPv4",
        )
    else:
        _checked_nmcli(
            "connection", "modify", "uuid", connection_uuid,
            "ipv4.method", "auto", "ipv4.addresses", "", "ipv4.gateway", "",
            "ipv4.dns", "", "ipv4.ignore-auto-dns", "no", "ipv4.dad-timeout", "-1",
            action="设置 DHCP",
        )


def _schedule_rollback(state: Dict[str, Any], rollback_seconds: int) -> None:
    systemd_run = command_path("systemd-run", "SYSTEMD_RUN_PATH")
    generation = int(state.get("rollback_generation", 0)) + 1
    unit = f"rk3588-network-rollback-{state['id']}-{generation}"
    command = [
        systemd_run, "--quiet", "--collect", "--unit", unit,
        f"--on-active={rollback_seconds}s", "--timer-property=AccuracySec=1s",
        f"--setenv=NETWORK_TRANSACTION_DIR={TRANSACTION_DIR}",
        f"--setenv=NMCLI_PATH={command_path('nmcli', 'NMCLI_PATH')}",
        f"--setenv=SYSTEMCTL_PATH={command_path('systemctl', 'SYSTEMCTL_PATH')}",
        sys.executable, str(Path(__file__).resolve()), "rollback", state["id"],
    ]
    result = run_command(command, timeout=15)
    if result.returncode != 0:
        raise NetworkManagerError((result.stderr or result.stdout or "无法启动网络回滚定时器").strip())
    state["rollback_generation"] = generation
    state["rollback_unit"] = unit


def _cancel_rollback_unit(unit: str) -> None:
    if not unit:
        return
    try:
        systemctl = command_path("systemctl", "SYSTEMCTL_PATH")
        run_command([systemctl, "stop", f"{unit}.timer"], timeout=10)
    except (FileNotFoundError, NetworkManagerError, subprocess.SubprocessError):
        pass


def _cancel_rollback(state: Dict[str, Any]) -> None:
    _cancel_rollback_unit(str(state.get("rollback_unit", "")))


def _active_uuid(device: str) -> str:
    values = _values("device", "GENERAL.CON-UUID", device)
    return values[0] if values and _UUID_RE.fullmatch(values[0]) else ""


def _new_transaction(device: str, old_uuid: str, new_uuid: str, final_name: str,
                     staged: bool, rollback_seconds: int, expected_address: str = "",
                     expected_gateway: str = "") -> Dict[str, Any]:
    rollback_seconds = max(MIN_ROLLBACK_SECONDS, min(MAX_ROLLBACK_SECONDS, int(rollback_seconds)))
    now = time.time()
    state = {
        "id": uuid_lib.uuid4().hex, "status": "scheduled", "device": device,
        "kind": "staged_profile" if staged else "saved_profile",
        "old_uuid": old_uuid, "new_uuid": new_uuid, "final_name": final_name,
        "original_name": "", "staged": staged, "created_at": now,
        "deadline": now + rollback_seconds, "target_addresses": [],
        "expected_gateway": expected_gateway, "rollback_seconds": rollback_seconds,
        "error": "",
    }
    if old_uuid:
        try:
            state["original_name"] = _profile_by_uuid(old_uuid)["name"]
        except NetworkManagerError:
            pass
    if expected_address:
        state["target_addresses"] = [expected_address]
    # 激活阶段也必须有回滚保护；自动检查通过后会重新开始完整确认倒计时。
    _schedule_rollback(state, rollback_seconds + ACTIVATION_ROLLBACK_GRACE_SECONDS)
    _write_transaction(state)
    return state


def start_network_change(config: Dict[str, Any]) -> Dict[str, Any]:
    device = _validate_device(str(config.get("device", "")))
    kind = str(config.get("type", ""))
    if kind not in ("ethernet", "wifi"):
        raise NetworkManagerError("网络类型只能是 ethernet 或 wifi")
    _assert_device(device, kind)
    ipv4_config = validate_ipv4(
        str(config.get("method", "")), str(config.get("address", "")),
        str(config.get("gateway", "")), config.get("dns", []),
    )

    with _transaction_lock():
        if _active_transaction():
            raise NetworkManagerError("已有网络切换正在等待确认，请先确认或回滚")
        old_uuid = _active_uuid(device)
        source_uuid = str(config.get("connection_uuid") or "").strip()
        if source_uuid:
            source = _profile_by_uuid(source_uuid)
            if source["type"] != kind:
                raise NetworkManagerError("连接配置类型与所选网卡不匹配")
        temp_name = f"web-test-{device}-{uuid_lib.uuid4().hex[:8]}"
        new_uuid = ""
        try:
            if source_uuid:
                _checked_nmcli(
                    "connection", "clone", "uuid", source_uuid, temp_name,
                    action="创建临时连接副本",
                )
            else:
                add_args = [
                    "connection", "add", "type", kind, "ifname", device,
                    "con-name", temp_name, "autoconnect", "no",
                ]
                if kind == "wifi":
                    ssid = str(config.get("ssid", "")).strip()
                    if not ssid or len(ssid.encode("utf-8")) > 32:
                        raise NetworkManagerError("Wi-Fi SSID 不能为空且不能超过 32 字节")
                    add_args.extend(["ssid", ssid])
                _checked_nmcli(*add_args, action="创建临时连接")

            uuid_values = nmcli("-g", "connection.uuid", "connection", "show", temp_name)
            if uuid_values.returncode != 0:
                raise NetworkManagerError("无法读取临时连接 UUID")
            new_uuid = uuid_values.stdout.strip().splitlines()[0]
            _validate_uuid(new_uuid)
            _checked_nmcli(
                "connection", "modify", "uuid", new_uuid,
                "connection.interface-name", device, "connection.autoconnect", "no",
                action="绑定临时连接网卡",
            )

            if kind == "wifi":
                ssid = str(config.get("ssid", "")).strip()
                if not ssid or len(ssid.encode("utf-8")) > 32:
                    raise NetworkManagerError("Wi-Fi SSID 不能为空且不能超过 32 字节")
                _checked_nmcli(
                    "connection", "modify", "uuid", new_uuid,
                    "802-11-wireless.ssid", ssid, action="设置 Wi-Fi SSID",
                )
                security = str(config.get("wifi_security", "wpa-psk"))
                password = str(config.get("wifi_password", ""))
                if security not in ("wpa-psk", "sae", "open"):
                    raise NetworkManagerError("Wi-Fi 安全方式不受支持")
                if security != "open":
                    if not password and source_uuid:
                        # 编辑当前 Wi-Fi 的 IPv4 时可沿用克隆配置中的现有密钥。
                        pass
                    else:
                        valid_hex_psk = len(password) == 64 and all(char in "0123456789abcdefABCDEF" for char in password)
                        if not valid_hex_psk and not 8 <= len(password) <= 63:
                            raise NetworkManagerError("Wi-Fi 密码必须为 8 到 63 个字符，或 64 位十六进制 PSK")
                        _checked_nmcli(
                            "connection", "modify", "uuid", new_uuid,
                            "wifi-sec.key-mgmt", security, "wifi-sec.psk", password,
                            action="设置 Wi-Fi 安全参数",
                        )
                elif not source_uuid:
                    # 新建的无线连接默认没有 wireless-security 设置，即为开放网络。
                    pass
                else:
                    _checked_nmcli(
                        "connection", "modify", "uuid", new_uuid,
                        "remove", "wifi-sec", action="清除 Wi-Fi 密码",
                    )

            _configure_ipv4(new_uuid, ipv4_config)
            source_name = _profile_by_uuid(source_uuid)["name"] if source_uuid else ""
            default_name = source_name or (
                f"Wi-Fi {str(config.get('ssid', '')).strip()}" if kind == "wifi" else f"LAN {device}"
            )
            requested_name = str(config.get("profile_name") or "").strip() or default_name
            final_name = _unique_profile_name(requested_name, ignored_uuid=source_uuid)
            expected_ip = ipv4_config["address"].split("/", 1)[0] if ipv4_config["address"] else ""
            state = _new_transaction(
                device, old_uuid, new_uuid, final_name, True,
                int(config.get("rollback_seconds", DEFAULT_ROLLBACK_SECONDS)),
                expected_ip, ipv4_config["gateway"],
            )
        except Exception:
            if new_uuid:
                nmcli("connection", "delete", "uuid", new_uuid)
            else:
                nmcli("connection", "delete", "id", temp_name)
            raise
    return _public_transaction(state)


def start_saved_connection(connection_uuid: str, device: str,
                           rollback_seconds: int = DEFAULT_ROLLBACK_SECONDS) -> Dict[str, Any]:
    profile = _profile_by_uuid(connection_uuid)
    device_row = _assert_device(device, profile["type"])
    if profile["active"]:
        raise NetworkManagerError(
            "该连接已经处于启用状态" if profile["device"] == device_row["device"]
            else f"该连接正在网卡 {profile['device']} 上使用，不能同时切换到其他网卡"
        )
    with _transaction_lock():
        if _active_transaction():
            raise NetworkManagerError("已有网络切换正在等待确认，请先确认或回滚")
        old_uuid = _active_uuid(device_row["device"])
        state = _new_transaction(
            device_row["device"], old_uuid, profile["uuid"], profile["name"],
            False, rollback_seconds,
        )
    return _public_transaction(state)


def activate_transaction(transaction_id: str) -> None:
    previous_rollback_unit = ""
    try:
        with _transaction_lock():
            state = _read_transaction(transaction_id)
            if state.get("status") != "scheduled":
                return
            state["status"] = "activating"
            _write_transaction(state)
        result = nmcli(
            "--wait", "45", "connection", "up", "uuid", state["new_uuid"],
            "ifname", state["device"], timeout=55,
        )
        if result.returncode != 0:
            raise NetworkManagerError((result.stderr or result.stdout or "新连接激活失败").strip())
        if _active_uuid(state["device"]) != state["new_uuid"]:
            raise NetworkManagerError("NetworkManager 没有切换到新的连接配置")
        addresses = _values("device", "IP4.ADDRESS", state["device"])
        if not addresses:
            raise NetworkManagerError("新连接没有获得 IPv4 地址")
        expected = state.get("target_addresses", [])
        if expected and not any(item.split("/", 1)[0] == expected[0] for item in addresses):
            raise NetworkManagerError(f"静态地址 {expected[0]} 没有生效")
        expected_gateway = state.get("expected_gateway", "")
        if expected_gateway:
            gateways = _values("device", "IP4.GATEWAY", state["device"])
            if expected_gateway not in gateways:
                raise NetworkManagerError(f"默认网关 {expected_gateway} 没有生效")
        with _transaction_lock():
            current = _read_transaction(transaction_id)
            if current.get("status") != "activating":
                return
            previous_rollback_unit = str(current.get("rollback_unit", ""))
            confirmation_seconds = int(current.get("rollback_seconds", DEFAULT_ROLLBACK_SECONDS))
            _schedule_rollback(current, confirmation_seconds)
            current["status"] = "awaiting_confirmation"
            current["target_addresses"] = addresses
            current["deadline"] = time.time() + confirmation_seconds
            _write_transaction(current)
        _cancel_rollback_unit(previous_rollback_unit)
    except Exception as exc:
        try:
            with _transaction_lock():
                current = _read_transaction(transaction_id)
                current["failure_reason"] = str(exc)
                _write_transaction(current)
            rollback_transaction(transaction_id)
        except Exception as rollback_exc:
            print(f"[Network] activation and rollback failed: {exc}; {rollback_exc}")


def _backup_name(name: str) -> str:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return _unique_profile_name(f"{name[:56]}.backup-{stamp}")


def confirm_transaction(transaction_id: str) -> Dict[str, Any]:
    with _transaction_lock():
        state = _read_transaction(transaction_id)
        if state.get("status") == "confirmed":
            return _public_transaction(state)
        if state.get("status") != "awaiting_confirmation":
            raise NetworkManagerError("新网络尚未通过自动检查，当前不能确认")
        if time.time() >= float(state.get("deadline", 0)):
            raise NetworkManagerError("确认时间已经结束，系统正在恢复原网络")
        state["status"] = "committing"
        _write_transaction(state)
        try:
            if state.get("staged"):
                old_uuid = state.get("old_uuid", "")
                if old_uuid and old_uuid != state["new_uuid"]:
                    old = _profile_by_uuid(old_uuid)
                    backup = _backup_name(old["name"])
                    _checked_nmcli(
                        "connection", "modify", "uuid", old_uuid,
                        "connection.id", backup, action="备份原连接配置",
                    )
                    state["backup_name"] = backup
                _checked_nmcli(
                    "connection", "modify", "uuid", state["new_uuid"],
                    "connection.id", state["final_name"],
                    "connection.autoconnect", "yes",
                    "connection.autoconnect-priority", "100",
                    action="保存新连接配置",
                )
                if old_uuid and old_uuid != state["new_uuid"]:
                    _checked_nmcli(
                        "connection", "modify", "uuid", old_uuid,
                        "connection.autoconnect", "no", action="停用旧连接自动连接",
                    )
            state["status"] = "confirmed"
            state["confirmed_at"] = time.time()
            _write_transaction(state)
        except Exception as exc:
            state["error"] = str(exc)
            state["status"] = "awaiting_confirmation"
            _write_transaction(state)
            raise
    _cancel_rollback(state)
    return _public_transaction(state)


def rollback_transaction(transaction_id: str) -> Dict[str, Any]:
    with _transaction_lock():
        state = _read_transaction(transaction_id)
        if state.get("status") in ("rolled_back", "confirmed"):
            return _public_transaction(state)
        state["status"] = "rolling_back"
        _write_transaction(state)
        errors: List[str] = []
        new_uuid = state.get("new_uuid", "")
        old_uuid = state.get("old_uuid", "")
        if new_uuid and _active_uuid(state["device"]) == new_uuid:
            result = nmcli("--wait", "15", "connection", "down", "uuid", new_uuid, timeout=20)
            if result.returncode != 0:
                errors.append((result.stderr or result.stdout).strip())
        if old_uuid and old_uuid != new_uuid:
            result = nmcli(
                "--wait", "30", "connection", "up", "uuid", old_uuid,
                "ifname", state["device"], timeout=40,
            )
            if result.returncode != 0:
                errors.append((result.stderr or result.stdout or "恢复原连接失败").strip())
            original_name = state.get("original_name", "")
            if original_name:
                restore = nmcli(
                    "connection", "modify", "uuid", old_uuid,
                    "connection.id", original_name, "connection.autoconnect", "yes",
                )
                if restore.returncode != 0:
                    errors.append((restore.stderr or restore.stdout or "恢复原连接名称失败").strip())
        if state.get("staged") and new_uuid:
            result = nmcli("connection", "delete", "uuid", new_uuid)
            if result.returncode != 0:
                errors.append((result.stderr or result.stdout or "清理临时连接失败").strip())
        state["status"] = "rollback_failed" if errors else "rolled_back"
        state["error"] = "; ".join(filter(None, errors))
        state["rolled_back_at"] = time.time()
        _write_transaction(state)
    _cancel_rollback(state)
    return _public_transaction(state)


def transaction_status(transaction_id: str) -> Dict[str, Any]:
    with _transaction_lock():
        return _public_transaction(_read_transaction(transaction_id))


def recover_incomplete_transactions() -> None:
    """控制台重启后核对回滚保护，绝不让半完成事务失去保护。"""
    state = _active_transaction()
    if not state:
        return
    status = state.get("status")
    if status == "awaiting_confirmation" and time.time() < float(state.get("deadline", 0)):
        unit = state.get("rollback_unit", "")
        if unit:
            try:
                result = run_command([
                    command_path("systemctl", "SYSTEMCTL_PATH"),
                    "is-active", f"{unit}.timer",
                ], timeout=10)
                if result.returncode == 0:
                    return
            except (FileNotFoundError, NetworkManagerError, subprocess.SubprocessError):
                pass
    # scheduled/activating/committing 表示原工作进程中途退出；回滚比猜测继续执行安全。
    rollback_transaction(state["id"])


def delete_connection(connection_uuid: str) -> None:
    with _transaction_lock():
        profile = _profile_by_uuid(connection_uuid)
        if profile["active"]:
            raise NetworkManagerError("不能删除正在使用的连接配置，请先安全切换到其他连接")
        active = _active_transaction()
        if active and connection_uuid in (active.get("old_uuid"), active.get("new_uuid")):
            raise NetworkManagerError("该连接正在参与网络切换，不能删除")
        _checked_nmcli("connection", "delete", "uuid", profile["uuid"], action="删除连接配置")


def ping_target(target: str) -> Dict[str, Any]:
    try:
        clean = str(ipaddress.IPv4Address(target.strip()))
    except ValueError as exc:
        raise NetworkManagerError("测试目标必须是合法的 IPv4 地址") from exc
    result = run_command([
        command_path("ping", "PING_PATH"), "-c", "3", "-W", "1", clean,
    ], timeout=8)
    return {
        "target": clean, "reachable": result.returncode == 0,
        "detail": (result.stdout or result.stderr).strip()[-2000:],
    }


def _main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] == "rollback":
        try:
            rollback_transaction(sys.argv[2])
            return 0
        except Exception as exc:
            print(f"[NetworkRollback] {exc}", file=sys.stderr)
            return 1
    print("This module is an internal Web Console network service.", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(_main())
