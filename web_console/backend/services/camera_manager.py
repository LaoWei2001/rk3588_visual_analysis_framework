"""摄像头配置的应用服务：规划、事务回滚、持久化和启动恢复。"""
from __future__ import annotations

import fcntl
import hashlib
import ipaddress
import json
import os
import re
import socket
import struct
import threading
import time
import uuid
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Dict, Iterator, List, Optional, Set

from .camera_discovery import CameraDiscovery, CameraDiscoveryError
from .camera_network_manager import CameraNetworkError, CameraNetworkManager
from .camera_status import CameraStatus, CameraStatusError
from .data_dir import APPS_ROOT


CONFIG_FILE = Path(os.environ.get(
    "CAMERA_CONFIG_FILE", str(APPS_ROOT / ".camera_network.json"),
))
LOCK_FILE = Path(os.environ.get(
    "CAMERA_CONFIG_LOCK", str(APPS_ROOT / ".camera_network.lock"),
))
TRANSACTION_FILE = Path(os.environ.get(
    "CAMERA_TRANSACTION_FILE", str(APPS_ROOT / ".camera_network_transaction.json"),
))
CONFIG_VERSION = 1
_LOCAL_LOCK = threading.RLock()
_MAC_RE = re.compile(r"^[0-9a-f]{2}(?::[0-9a-f]{2}){5}$")


class CameraManagerError(RuntimeError):
    pass


@contextmanager
def _config_lock() -> Iterator[None]:
    LOCK_FILE.parent.mkdir(parents=True, exist_ok=True)
    with _LOCAL_LOCK, LOCK_FILE.open("a+", encoding="utf-8") as handle:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


def _read_config_unlocked() -> Optional[Dict[str, Any]]:
    try:
        value = json.loads(CONFIG_FILE.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, ValueError, TypeError) as exc:
        raise CameraManagerError(f"摄像头持久化配置损坏：{exc}") from exc
    if not isinstance(value, dict) or value.get("version") != CONFIG_VERSION:
        raise CameraManagerError("摄像头持久化配置版本无效")
    required = ("interface", "camera_ip", "camera_prefix_length", "local_ip")
    if not all(value.get(key) not in (None, "") for key in required):
        raise CameraManagerError("摄像头持久化配置缺少必要字段")
    return value


def read_configuration() -> Optional[Dict[str, Any]]:
    with _config_lock():
        return _read_config_unlocked()


def _write_json_atomic(path: Path, value: Dict[str, Any], label: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(f"{path.suffix}.tmp")
    payload = json.dumps(value, ensure_ascii=False, indent=2) + "\n"
    try:
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
        os.chmod(path, 0o600)
    except OSError as exc:
        try:
            temporary.unlink()
        except OSError:
            pass
        raise CameraManagerError(f"保存{label}失败：{exc}") from exc


def _write_config_unlocked(value: Dict[str, Any]) -> None:
    _write_json_atomic(CONFIG_FILE, value, "摄像头配置")


def _read_transaction_unlocked() -> Optional[Dict[str, Any]]:
    try:
        value = json.loads(TRANSACTION_FILE.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    except (OSError, ValueError, TypeError) as exc:
        raise CameraManagerError(f"摄像头网络事务记录损坏：{exc}") from exc
    if not isinstance(value, dict) or not value.get("id") or not isinstance(value.get("new"), dict):
        raise CameraManagerError("摄像头网络事务记录缺少必要字段")
    return value


def _write_transaction_unlocked(value: Dict[str, Any]) -> None:
    _write_json_atomic(TRANSACTION_FILE, value, "摄像头网络事务")


def _clear_transaction_unlocked() -> None:
    try:
        TRANSACTION_FILE.unlink()
    except FileNotFoundError:
        pass
    except OSError as exc:
        raise CameraManagerError(f"清理摄像头网络事务记录失败：{exc}") from exc


def _normalize_mac(value: Any) -> str:
    raw = str(value or "").strip().lower().replace("-", ":")
    if not raw:
        return ""
    if not _MAC_RE.fullmatch(raw):
        raise CameraManagerError("摄像头 MAC 格式无效")
    return raw


def _ipv4(value: Any, label: str) -> ipaddress.IPv4Address:
    try:
        address = ipaddress.IPv4Address(str(value).strip())
    except ValueError as exc:
        raise CameraManagerError(f"{label}不是合法 IPv4 地址") from exc
    if address.is_unspecified or address.is_multicast or address.is_loopback:
        raise CameraManagerError(f"{label}不能使用保留、组播或回环地址")
    return address


def _port(value: Any, label: str) -> int:
    try:
        port = int(value)
    except (TypeError, ValueError) as exc:
        raise CameraManagerError(f"{label}不是有效端口") from exc
    if not 1 <= port <= 65535:
        raise CameraManagerError(f"{label}必须在 1 到 65535 之间")
    return port


def _prefix(value: Any) -> int:
    try:
        prefix = int(value)
    except (TypeError, ValueError) as exc:
        raise CameraManagerError("摄像头子网前缀无效") from exc
    # /31 和 /32 没有可供 RK3588 使用的另一个常规主机地址。
    if not 1 <= prefix <= 30:
        raise CameraManagerError("摄像头子网前缀必须在 1 到 30 之间")
    return prefix


def _all_local_ips(all_addresses: Dict[str, List[str]]) -> Set[ipaddress.IPv4Address]:
    result: Set[ipaddress.IPv4Address] = set()
    for values in all_addresses.values():
        for value in values:
            try:
                result.add(ipaddress.IPv4Interface(value).ip)
            except ValueError:
                continue
    return result


def _resolver_ips() -> Set[ipaddress.IPv4Address]:
    result: Set[ipaddress.IPv4Address] = set()
    try:
        lines = Path("/etc/resolv.conf").read_text(encoding="utf-8").splitlines()
    except OSError:
        return result
    for line in lines:
        fields = line.split()
        if len(fields) >= 2 and fields[0] == "nameserver":
            try:
                result.add(ipaddress.IPv4Address(fields[1]))
            except ValueError:
                pass
    return result


def _arp_neighbors() -> List[Dict[str, str]]:
    try:
        lines = Path("/proc/net/arp").read_text(encoding="ascii").splitlines()[1:]
    except OSError:
        return []
    result: List[Dict[str, str]] = []
    for line in lines:
        fields = line.split()
        if len(fields) < 6:
            continue
        try:
            valid = int(fields[2], 16) & 0x2
            ipaddress.IPv4Address(fields[0])
        except ValueError:
            continue
        if valid and fields[3] != "00:00:00:00:00:00":
            result.append({"ip": fields[0], "mac": fields[3].lower(), "interface": fields[5]})
    return result


def _active_remote_ipv4s() -> Set[ipaddress.IPv4Address]:
    """读取当前 TCP/UDP 远端，避免精确主机路由截断已有服务器连接。"""
    result: Set[ipaddress.IPv4Address] = set()
    for table in ("/proc/net/tcp", "/proc/net/udp"):
        try:
            lines = Path(table).read_text(encoding="ascii").splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            fields = line.split()
            if len(fields) < 4 or ":" not in fields[2]:
                continue
            raw_ip, raw_port = fields[2].split(":", 1)
            if raw_ip == "00000000" or raw_port == "0000":
                continue
            try:
                packed = struct.pack("<I", int(raw_ip, 16))
                result.add(ipaddress.IPv4Address(socket.inet_ntoa(packed)))
            except (OSError, ValueError, struct.error):
                continue
    return result


def _network_conflicts(network: ipaddress.IPv4Network, selected: str,
                       all_addresses: Dict[str, List[str]]) -> List[Dict[str, str]]:
    conflicts: List[Dict[str, str]] = []
    seen = set()
    for device, values in all_addresses.items():
        if device == selected:
            continue
        for value in values:
            try:
                other = ipaddress.IPv4Interface(value).network
            except ValueError:
                continue
            if not network.overlaps(other):
                continue
            key = (device, str(other))
            if key in seen:
                continue
            seen.add(key)
            conflicts.append({
                "interface": device,
                "network": str(other),
                "address": value,
            })
    return conflicts


def _assert_camera_ip_safe(camera_ip: ipaddress.IPv4Address, selected: str,
                           all_addresses: Dict[str, List[str]], routes: List[Dict[str, Any]],
                           management_peer: str, camera_mac: str = "",
                           managed_current: Optional[Dict[str, Any]] = None) -> None:
    local_ips = _all_local_ips(all_addresses)
    if camera_ip in local_ips:
        raise CameraManagerError(f"摄像头 IP {camera_ip} 与 RK3588 本机地址完全相同，拒绝应用")
    protected: Dict[ipaddress.IPv4Address, str] = {}
    for route in routes:
        if route.get("destination") == "0.0.0.0" and route.get("prefix_length") == 0:
            gateway = route.get("gateway")
            if gateway:
                try:
                    protected[ipaddress.IPv4Address(gateway)] = f"网卡 {route['interface']} 的默认网关"
                except ValueError:
                    pass
    for resolver in _resolver_ips():
        protected[resolver] = "系统 DNS 服务器"
    try:
        peer = ipaddress.IPv4Address(management_peer)
        protected[peer] = "当前 Web 管理客户端"
    except ValueError:
        pass
    if camera_ip in protected:
        raise CameraManagerError(
            f"摄像头 IP {camera_ip} 与{protected[camera_ip]}完全相同；继续会中断原网络"
        )
    exact_route_on_selected = any(
        route.get("destination") == str(camera_ip)
        and route.get("prefix_length") == 32
        and route.get("interface") == selected
        for route in routes
    )
    managed_same_camera = bool(
        managed_current
        and managed_current.get("camera_ip") == str(camera_ip)
        and managed_current.get("route_owned")
    )
    if camera_ip in _active_remote_ipv4s() and not exact_route_on_selected and not managed_same_camera:
        raise CameraManagerError(
            f"摄像头 IP {camera_ip} 正被当前 TCP/UDP 服务器连接使用；拒绝改变其出口网口"
        )
    neighbors = [neighbor for neighbor in _arp_neighbors() if neighbor["ip"] == str(camera_ip)]
    collision = next((
        neighbor for neighbor in neighbors
        if neighbor["interface"] != selected
        and not (
            managed_same_camera
            and neighbor["interface"] == managed_current.get("interface")
            and (not camera_mac or neighbor["mac"] == camera_mac)
        )
    ), None)
    if collision:
        raise CameraManagerError(
            f"摄像头 IP {camera_ip} 已在网卡 {collision['interface']} 对应另一台设备 "
            f"({collision['mac']})；精确 IP 冲突无法无损隔离"
        )
    unexpected = next((
        neighbor for neighbor in neighbors
        if neighbor["interface"] == selected and camera_mac and neighbor["mac"] != camera_mac
    ), None)
    if unexpected:
        raise CameraManagerError(
            f"网口 {selected} 上的 {camera_ip} 实际 MAC 为 {unexpected['mac']}，"
            f"与所选摄像头 {camera_mac} 不符"
        )


def _candidate_addresses(network: ipaddress.IPv4Network, seed_text: str,
                         limit: int = 32) -> List[ipaddress.IPv4Address]:
    usable = int(network.num_addresses) - 2
    if usable <= 0:
        return []
    digest = hashlib.sha256(seed_text.encode("utf-8")).digest()
    start = int.from_bytes(digest[:8], "big") % usable
    count = min(usable, limit)
    base = int(network.network_address) + 1
    return [ipaddress.IPv4Address(base + ((start + index) % usable)) for index in range(count)]


def _select_local_ip(device: str, camera_ip: ipaddress.IPv4Address,
                     network: ipaddress.IPv4Network, camera_mac: str,
                     selectable_interfaces: List[Dict[str, Any]],
                     all_addresses: Dict[str, List[str]],
                     current: Optional[Dict[str, Any]]) -> tuple[str, List[str]]:
    selected = next((item for item in selectable_interfaces if item["device"] == device), None)
    if selected is None:
        raise CameraManagerError(f"网卡 {device} 不是可选择的物理有线网口")
    warnings: List[str] = []

    preferred: List[ipaddress.IPv4Interface] = []
    if current and current.get("interface") == device:
        try:
            preferred.append(ipaddress.IPv4Interface(f"{current['local_ip']}/32"))
        except ValueError:
            pass
    parsed_selected: List[ipaddress.IPv4Interface] = []
    for value in selected.get("addresses", []):
        try:
            parsed_selected.append(ipaddress.IPv4Interface(value))
        except ValueError:
            continue
    # 已有 /32 通常就是历史摄像头侧地址，优先于该网口的普通业务地址。
    preferred.extend(sorted(parsed_selected, key=lambda item: item.network.prefixlen, reverse=True))
    seen = set()
    for interface in preferred:
        if interface.ip in seen:
            continue
        seen.add(interface.ip)
        if interface.ip != camera_ip and interface.ip in network:
            return str(interface.ip), warnings

    occupied = _all_local_ips(all_addresses)
    seed = f"{device}|{selected.get('mac', '')}|{camera_ip}|{camera_mac}"
    candidates = _candidate_addresses(network, seed)
    if not selected.get("link_up"):
        warnings.append("所选网口当前没有物理 Link，无法执行 ARP 地址占用探测")
    for candidate in candidates:
        if (candidate == camera_ip or candidate in occupied or candidate.is_unspecified
                or candidate.is_loopback or candidate.is_multicast):
            continue
        if selected.get("link_up"):
            try:
                in_use = CameraNetworkManager.arp_probe(
                    device, str(candidate), source_ip="0.0.0.0", timeout_seconds=0.18,
                )
            except CameraNetworkError as exc:
                raise CameraManagerError(f"自动选择本地 IP 时 ARP 探测失败：{exc}") from exc
            if in_use:
                continue
        return str(candidate), warnings
    raise CameraManagerError("无法在摄像头子网中找到未占用的 RK3588 本地 IP")


def plan_configuration(payload: Dict[str, Any], management_peer: str = "",
                       *, current: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    device = str(payload.get("interface", "")).strip()
    camera_ip = _ipv4(payload.get("camera_ip", ""), "摄像头 IP")
    prefix = _prefix(payload.get("prefix_length"))
    network = ipaddress.IPv4Network(f"{camera_ip}/{prefix}", strict=False)
    if camera_ip in (network.network_address, network.broadcast_address):
        raise CameraManagerError("摄像头 IP 不能是网段地址或广播地址")
    camera_mac = _normalize_mac(payload.get("camera_mac", ""))
    http_port = _port(payload.get("http_port", 80), "HTTP 端口")
    rtsp_port = _port(payload.get("rtsp_port", 554), "RTSP 端口")
    try:
        selectable = CameraNetworkManager.interfaces()
        all_addresses = CameraNetworkManager.all_ipv4_addresses()
        routes = CameraNetworkManager.routes()
    except CameraNetworkError as exc:
        raise CameraManagerError(str(exc)) from exc
    selected = next((item for item in selectable if item["device"] == device), None)
    if selected is None:
        raise CameraManagerError(f"网卡 {device} 不是可选择的物理有线网口")
    _assert_camera_ip_safe(
        camera_ip, device, all_addresses, routes, management_peer, camera_mac, current,
    )
    conflicts = _network_conflicts(network, device, all_addresses)
    local_ip, warnings = _select_local_ip(
        device, camera_ip, network, camera_mac, selectable, all_addresses, current,
    )
    try:
        state = CameraNetworkManager.network_state(device, local_ip, str(camera_ip))
    except CameraNetworkError as exc:
        raise CameraManagerError(str(exc)) from exc
    managed_old_route = bool(
        current
        and current.get("route_owned")
        and current.get("camera_ip") == str(camera_ip)
        and state["route_interface"] == current.get("interface")
    )
    if state["route_interface"] not in (None, device) and not managed_old_route:
        raise CameraManagerError(
            f"摄像头 IP 已有精确路由指向 {state['route_interface']}，拒绝覆盖原路由"
        )
    if conflicts:
        warnings.append(
            "摄像头网段与现有网络重叠；将使用 /32 本地地址和 /32 主机路由，仅把该摄像头 IP 定向到所选网口"
        )
    if selected.get("has_default_route"):
        warnings.append("所选网口承载默认路由；应用时只增加隔离地址和主机路由，不修改其现有连接")
    if payload.get("http_port_inferred"):
        warnings.append("HTTP 端口未由发现报文明确广播，当前值为推断值，可在应用前修改")
    if payload.get("rtsp_port_inferred"):
        warnings.append("RTSP 端口未由发现报文明确广播，当前值为推断值，可在应用前修改")
    return {
        "interface": device,
        "camera_ip": str(camera_ip),
        "camera_prefix_length": prefix,
        "camera_network": str(network),
        "camera_mac": camera_mac,
        "model": str(payload.get("model", "")).strip()[:160],
        "serial": str(payload.get("serial", "")).strip()[:160],
        "http_port": http_port,
        "rtsp_port": rtsp_port,
        "http_port_inferred": bool(payload.get("http_port_inferred", False)),
        "rtsp_port_inferred": bool(payload.get("rtsp_port_inferred", False)),
        "local_ip": local_ip,
        "local_prefix_length": 32,
        "isolation_mode": "host-route",
        "conflicts": conflicts,
        "warnings": warnings,
        "address_preexisting": bool(state["address_on_interface"]),
        "route_preexisting": state["route_interface"] == device,
    }


def discover_cameras(device: str, timeout_seconds: float = 3.0) -> List[Dict[str, Any]]:
    selectable = {item["device"] for item in CameraNetworkManager.interfaces()}
    if device not in selectable:
        raise CameraManagerError(f"网卡 {device} 不是可选择的物理有线网口")
    try:
        return CameraDiscovery.discover(device, timeout_seconds)
    except CameraDiscoveryError as exc:
        raise CameraManagerError(str(exc)) from exc


def _same_resources(left: Optional[Dict[str, Any]], right: Dict[str, Any]) -> bool:
    if not left:
        return False
    return all(left.get(key) == right.get(key) for key in ("interface", "local_ip", "camera_ip"))


def _remove_resources(config: Dict[str, Any]) -> None:
    try:
        if config["interface"] not in {item["device"] for item in CameraNetworkManager.interfaces()}:
            # 网卡已经拔除/改名时，其内核地址和路由也已不存在；可安全清理持久态。
            return
        CameraNetworkManager.remove(
            config["interface"], config["local_ip"], config["camera_ip"],
            remove_address=bool(config.get("address_owned", False)),
            remove_route=bool(config.get("route_owned", False)),
        )
    except CameraNetworkError as exc:
        raise CameraManagerError(str(exc)) from exc


def _restore_resources(config: Dict[str, Any]) -> None:
    try:
        CameraNetworkManager.apply(config["interface"], config["local_ip"], config["camera_ip"])
    except CameraNetworkError as exc:
        raise CameraManagerError(str(exc)) from exc


def _rollback_pending_transaction_unlocked(transaction: Dict[str, Any]) -> None:
    errors: List[str] = []
    new_config = transaction["new"]
    try:
        _remove_resources(new_config)
    except Exception as exc:
        errors.append(f"清理未提交的新配置失败：{exc}")
    old_config = transaction.get("old")
    if isinstance(old_config, dict):
        try:
            _restore_resources(old_config)
        except Exception as exc:
            errors.append(f"恢复事务前配置失败：{exc}")
    if errors:
        # 保留事务文件，下一次 systemd 重启仍可继续恢复。
        raise CameraManagerError("；".join(errors))
    _clear_transaction_unlocked()


def _recover_pending_transaction_unlocked() -> None:
    transaction = _read_transaction_unlocked()
    if not transaction:
        return
    committed = _read_config_unlocked()
    if committed and committed.get("transaction_id") == transaction["id"]:
        _clear_transaction_unlocked()
        return
    _rollback_pending_transaction_unlocked(transaction)


def apply_configuration(payload: Dict[str, Any], management_peer: str = "") -> Dict[str, Any]:
    with _config_lock():
        _recover_pending_transaction_unlocked()
        old = _read_config_unlocked()
        plan = plan_configuration(payload, management_peer, current=old)
        same_resources = _same_resources(old, plan)
        address_will_be_owned = not plan["address_preexisting"]
        route_will_be_owned = not plan["route_preexisting"]
        if old and not same_resources:
            if (old.get("address_owned") and old.get("interface") == plan["interface"]
                    and old.get("local_ip") == plan["local_ip"]):
                address_will_be_owned = True
            if (old.get("route_owned") and old.get("interface") == plan["interface"]
                    and old.get("camera_ip") == plan["camera_ip"]):
                route_will_be_owned = True
        transaction_id = uuid.uuid4().hex
        transaction = {
            "id": transaction_id,
            "created_at": time.time(),
            "old": old,
            "new": {
                **plan,
                "address_owned": address_will_be_owned,
                "route_owned": route_will_be_owned,
            },
        }
        # 事务日志必须先落盘；进程在任意后续指令间退出，启动恢复都能识别并回滚。
        _write_transaction_unlocked(transaction)
        old_removed = False
        apply_result: Optional[Dict[str, bool]] = None
        try:
            if old and not same_resources:
                old_removed = bool(old.get("address_owned") or old.get("route_owned"))
                _remove_resources(old)
            apply_result = CameraNetworkManager.apply(
                plan["interface"], plan["local_ip"], plan["camera_ip"],
            )
            state = CameraNetworkManager.network_state(
                plan["interface"], plan["local_ip"], plan["camera_ip"],
            )
            if not state["address_on_interface"] or state["route_interface"] != plan["interface"]:
                raise CameraManagerError("Netlink 返回成功，但摄像头隔离地址或主机路由未生效")
            config = {
                "version": CONFIG_VERSION,
                **plan,
                "address_owned": (
                    bool(old.get("address_owned")) if same_resources and old
                    else not apply_result["address_preexisting"]
                ),
                "route_owned": (
                    bool(old.get("route_owned")) if same_resources and old
                    else not apply_result["route_preexisting"]
                ),
                "applied_at": time.time(),
                "transaction_id": transaction_id,
            }
            _write_config_unlocked(config)
            try:
                _clear_transaction_unlocked()
            except CameraManagerError as clear_exc:
                # 配置已原子提交；保留日志比错误回滚更安全，启动时会按 transaction_id 清理。
                print(f"[CameraNetwork] {clear_exc}")
            return config
        except Exception as exc:
            rollback_errors: List[str] = []
            if apply_result is not None and not same_resources:
                try:
                    CameraNetworkManager.remove(
                        plan["interface"], plan["local_ip"], plan["camera_ip"],
                        remove_address=not apply_result["address_preexisting"],
                        remove_route=not apply_result["route_preexisting"],
                    )
                except Exception as cleanup_exc:
                    rollback_errors.append(f"清理新配置失败：{cleanup_exc}")
            if old and old_removed:
                try:
                    _restore_resources(old)
                except Exception as restore_exc:
                    rollback_errors.append(f"恢复旧配置失败：{restore_exc}")
            if not rollback_errors:
                try:
                    _clear_transaction_unlocked()
                except Exception as clear_exc:
                    rollback_errors.append(f"清理事务记录失败：{clear_exc}")
            detail = str(exc)
            if rollback_errors:
                detail += "；" + "；".join(rollback_errors)
            if isinstance(exc, CameraManagerError):
                raise CameraManagerError(detail) from exc
            if isinstance(exc, CameraNetworkError):
                raise CameraManagerError(detail) from exc
            raise CameraManagerError(f"应用摄像头配置失败：{detail}") from exc


def remove_configuration() -> None:
    with _config_lock():
        _recover_pending_transaction_unlocked()
        config = _read_config_unlocked()
        if not config:
            return
        _remove_resources(config)
        try:
            CONFIG_FILE.unlink()
        except FileNotFoundError:
            pass
        except OSError as exc:
            # 文件仍在，重启时会自动恢复，避免留下“运行态已删但持久态不明”的状态。
            try:
                _restore_resources(config)
            except Exception as restore_exc:
                raise CameraManagerError(
                    f"删除持久化配置失败：{exc}；恢复运行配置也失败：{restore_exc}"
                ) from exc
            raise CameraManagerError(f"删除持久化配置失败：{exc}，已恢复运行配置") from exc


def restore_persisted_configuration() -> None:
    """控制台启动时幂等恢复；发现精确 IP 风险时宁可不应用。"""
    with _config_lock():
        _recover_pending_transaction_unlocked()
        config = _read_config_unlocked()
        if not config:
            return
        try:
            selectable = CameraNetworkManager.interfaces()
            if config["interface"] not in {item["device"] for item in selectable}:
                raise CameraManagerError(f"持久化网卡 {config['interface']} 不存在")
            all_addresses = CameraNetworkManager.all_ipv4_addresses()
            routes = CameraNetworkManager.routes()
            _assert_camera_ip_safe(
                ipaddress.IPv4Address(config["camera_ip"]), config["interface"],
                all_addresses, routes, "", config.get("camera_mac", ""), config,
            )
            result = CameraNetworkManager.apply(
                config["interface"], config["local_ip"], config["camera_ip"],
            )
        except (ValueError, CameraNetworkError) as exc:
            raise CameraManagerError(f"恢复持久化摄像头配置失败：{exc}") from exc
        changed = False
        if not result["address_preexisting"] and not config.get("address_owned"):
            config["address_owned"] = True
            changed = True
        if not result["route_preexisting"] and not config.get("route_owned"):
            config["route_owned"] = True
            changed = True
        if changed:
            config["restored_at"] = time.time()
            _write_config_unlocked(config)


def configuration_status(config: Optional[Dict[str, Any]] = None) -> Optional[Dict[str, Any]]:
    current = config if config is not None else read_configuration()
    if not current:
        return None
    try:
        status = CameraStatus.check(
            current["interface"], current["local_ip"], current["camera_ip"],
            current["http_port"], current["rtsp_port"], timeout_seconds=0.8,
        )
    except CameraStatusError as exc:
        raise CameraManagerError(str(exc)) from exc
    expected_mac = current.get("camera_mac", "")
    status["mac_matches"] = (
        None if not expected_mac or not status.get("arp_mac")
        else status["arp_mac"].lower() == expected_mac.lower()
    )
    return status


def snapshot(*, include_status: bool = True) -> Dict[str, Any]:
    try:
        interfaces = CameraNetworkManager.interfaces()
    except CameraNetworkError as exc:
        return {"interfaces": [], "configuration": None, "status": None, "error": str(exc)}
    try:
        config = read_configuration()
    except CameraManagerError as exc:
        return {"interfaces": interfaces, "configuration": None, "status": None, "error": str(exc)}
    for interface in interfaces:
        interface["configured"] = bool(config and config.get("interface") == interface["device"])
    status = None
    status_error = None
    if config and include_status:
        try:
            status = configuration_status(config)
        except CameraManagerError as exc:
            status_error = str(exc)
    return {
        "interfaces": interfaces,
        "configuration": config,
        "status": status,
        "status_error": status_error,
        "error": None,
    }
