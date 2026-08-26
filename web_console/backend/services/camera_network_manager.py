"""摄像头专用网口的安全网络管理。

只通过 rtnetlink 添加/删除一个本地 ``/32`` 地址和一个摄像头 ``/32`` 主机路由；
不会设置网关、DNS、默认路由，也不会修改 NetworkManager 连接配置。这样即使摄像
头网段与 wlan0/eth0 重叠，也只有摄像头这个单一目标地址走指定网口。
"""
from __future__ import annotations

import errno
import ipaddress
import os
import re
import select
import socket
import struct
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


_DEVICE_RE = re.compile(r"^[A-Za-z0-9_.:-]{1,15}$")
_SYS_NET = Path("/sys/class/net")

# rtnetlink constants (linux/netlink.h, linux/rtnetlink.h)
_NETLINK_ROUTE = 0
_NLMSG_ERROR = 2
_NLMSG_DONE = 3
_NLM_F_REQUEST = 0x0001
_NLM_F_ACK = 0x0004
_NLM_F_ROOT = 0x0100
_NLM_F_MATCH = 0x0200
_NLM_F_DUMP = _NLM_F_ROOT | _NLM_F_MATCH
_NLM_F_EXCL = 0x0200
_NLM_F_CREATE = 0x0400
_RTM_NEWADDR = 20
_RTM_DELADDR = 21
_RTM_GETADDR = 22
_RTM_NEWROUTE = 24
_RTM_DELROUTE = 25
_IFA_ADDRESS = 1
_IFA_LOCAL = 2
_RTA_DST = 1
_RTA_OIF = 4
_RTA_PREFSRC = 7
_RT_TABLE_MAIN = 254
_RTPROT_STATIC = 4
_RT_SCOPE_UNIVERSE = 0
_RT_SCOPE_LINK = 253
_RTN_UNICAST = 1

_NLMSG_HEADER = struct.Struct("=IHHII")
_IFADDR_MESSAGE = struct.Struct("=BBBBI")
_ROUTE_MESSAGE = struct.Struct("=BBBBBBBBI")
_ATTRIBUTE_HEADER = struct.Struct("=HH")

_ETH_P_ARP = 0x0806
_ETH_P_IP = 0x0800
_ARPHRD_ETHER = 1
_ARPOP_REQUEST = 1
_ARPOP_REPLY = 2


class CameraNetworkError(RuntimeError):
    pass


def _align(value: int, boundary: int = 4) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def _attribute(kind: int, payload: bytes) -> bytes:
    length = _ATTRIBUTE_HEADER.size + len(payload)
    return _ATTRIBUTE_HEADER.pack(length, kind) + payload + b"\0" * (_align(length) - length)


def _parse_attributes(payload: bytes, offset: int) -> Dict[int, bytes]:
    result: Dict[int, bytes] = {}
    while offset + _ATTRIBUTE_HEADER.size <= len(payload):
        length, kind = _ATTRIBUTE_HEADER.unpack_from(payload, offset)
        if length < _ATTRIBUTE_HEADER.size or offset + length > len(payload):
            break
        result[kind] = payload[offset + _ATTRIBUTE_HEADER.size:offset + length]
        offset += _align(length)
    return result


def _netlink_message(message_type: int, flags: int, payload: bytes,
                     *, timeout: float = 3.0) -> None:
    sequence = (time.monotonic_ns() ^ os.getpid()) & 0xFFFFFFFF
    message = _NLMSG_HEADER.pack(
        _NLMSG_HEADER.size + len(payload), message_type,
        flags | _NLM_F_REQUEST | _NLM_F_ACK, sequence, 0,
    ) + payload
    try:
        with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, _NETLINK_ROUTE) as netlink:
            netlink.settimeout(timeout)
            netlink.bind((0, 0))
            netlink.send(message)
            while True:
                response = netlink.recv(65535)
                offset = 0
                while offset + _NLMSG_HEADER.size <= len(response):
                    length, kind, _, reply_sequence, _ = _NLMSG_HEADER.unpack_from(response, offset)
                    if length < _NLMSG_HEADER.size:
                        raise CameraNetworkError("收到损坏的 Netlink 应答")
                    body = response[offset + _NLMSG_HEADER.size:offset + length]
                    offset += _align(length)
                    if reply_sequence != sequence:
                        continue
                    if kind == _NLMSG_ERROR:
                        if len(body) < 4:
                            raise CameraNetworkError("Netlink 错误应答不完整")
                        error_code = -struct.unpack_from("=i", body)[0]
                        if error_code:
                            raise OSError(error_code, os.strerror(error_code))
                        return
                    if kind == _NLMSG_DONE:
                        return
    except (OSError, TimeoutError) as exc:
        if isinstance(exc, CameraNetworkError):
            raise
        raise CameraNetworkError(f"Netlink 网络操作失败：{exc}") from exc


def _dump_ipv4_addresses() -> Dict[str, List[str]]:
    sequence = (time.monotonic_ns() ^ os.getpid()) & 0xFFFFFFFF
    payload = _IFADDR_MESSAGE.pack(socket.AF_INET, 0, 0, 0, 0)
    request = _NLMSG_HEADER.pack(
        _NLMSG_HEADER.size + len(payload), _RTM_GETADDR,
        _NLM_F_REQUEST | _NLM_F_DUMP, sequence, 0,
    ) + payload
    addresses: Dict[str, List[str]] = {}
    try:
        with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, _NETLINK_ROUTE) as netlink:
            netlink.settimeout(3.0)
            netlink.bind((0, 0))
            netlink.send(request)
            complete = False
            while not complete:
                response = netlink.recv(65535)
                offset = 0
                while offset + _NLMSG_HEADER.size <= len(response):
                    length, kind, _, reply_sequence, _ = _NLMSG_HEADER.unpack_from(response, offset)
                    if length < _NLMSG_HEADER.size:
                        break
                    body = response[offset + _NLMSG_HEADER.size:offset + length]
                    offset += _align(length)
                    if reply_sequence != sequence:
                        continue
                    if kind == _NLMSG_DONE:
                        complete = True
                        break
                    if kind == _NLMSG_ERROR:
                        error_code = -struct.unpack_from("=i", body)[0] if len(body) >= 4 else errno.EIO
                        if error_code:
                            raise OSError(error_code, os.strerror(error_code))
                        continue
                    if kind != _RTM_NEWADDR or len(body) < _IFADDR_MESSAGE.size:
                        continue
                    family, prefix, _, _, ifindex = _IFADDR_MESSAGE.unpack_from(body)
                    if family != socket.AF_INET:
                        continue
                    attributes = _parse_attributes(body, _IFADDR_MESSAGE.size)
                    raw_address = attributes.get(_IFA_LOCAL) or attributes.get(_IFA_ADDRESS)
                    if raw_address is None or len(raw_address) < 4:
                        continue
                    try:
                        name = socket.if_indextoname(ifindex)
                    except OSError:
                        continue
                    address = socket.inet_ntop(socket.AF_INET, raw_address[:4])
                    addresses.setdefault(name, []).append(f"{address}/{prefix}")
    except (OSError, TimeoutError) as exc:
        raise CameraNetworkError(f"读取 IPv4 地址失败：{exc}") from exc
    return addresses


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except (OSError, UnicodeError):
        return ""


def _physical_ethernet_names() -> List[str]:
    result: List[str] = []
    try:
        entries = list(_SYS_NET.iterdir())
    except OSError as exc:
        raise CameraNetworkError(f"无法枚举网卡：{exc}") from exc
    for entry in entries:
        name = entry.name
        if name == "lo" or not _DEVICE_RE.fullmatch(name):
            continue
        if _read_text(entry / "type") != "1" or not (entry / "device").exists():
            continue
        if (entry / "wireless").exists() or "DEVTYPE=gadget" in _read_text(entry / "uevent"):
            continue
        result.append(name)
    return sorted(result)


def _route_rows() -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = []
    try:
        lines = Path("/proc/net/route").read_text(encoding="ascii").splitlines()[1:]
    except OSError as exc:
        raise CameraNetworkError(f"无法读取内核路由表：{exc}") from exc
    for line in lines:
        fields = line.split()
        if len(fields) < 8:
            continue
        try:
            destination_value = int(fields[1], 16)
            gateway_value = int(fields[2], 16)
            flags = int(fields[3], 16)
            metric = int(fields[6])
            mask_value = int(fields[7], 16)
            destination = socket.inet_ntoa(struct.pack("=I", destination_value))
            gateway = socket.inet_ntoa(struct.pack("=I", gateway_value))
            mask = socket.inet_ntoa(struct.pack("=I", mask_value))
            prefix = ipaddress.IPv4Network(f"0.0.0.0/{mask}").prefixlen
        except (OSError, ValueError, struct.error):
            continue
        if not flags & 0x1:
            continue
        rows.append({
            "interface": fields[0],
            "destination": destination,
            "prefix_length": prefix,
            "gateway": "" if gateway == "0.0.0.0" else gateway,
            "metric": metric,
        })
    return rows


def _validate_ipv4(value: str, label: str) -> str:
    try:
        address = ipaddress.IPv4Address(str(value).strip())
    except ValueError as exc:
        raise CameraNetworkError(f"{label}不是合法 IPv4 地址") from exc
    if address.is_unspecified or address.is_multicast or address.is_loopback:
        raise CameraNetworkError(f"{label}不能使用保留、组播或回环地址")
    return str(address)


def _validate_device(device: str) -> str:
    clean = str(device).strip()
    if not _DEVICE_RE.fullmatch(clean):
        raise CameraNetworkError("非法的网卡名称")
    if clean not in _physical_ethernet_names():
        raise CameraNetworkError(f"网卡 {clean} 不是可用的物理有线网口")
    return clean


def _address_owner(ip: str) -> Optional[str]:
    for device, values in _dump_ipv4_addresses().items():
        if any(value.split("/", 1)[0] == ip for value in values):
            return device
    return None


def _exact_route_owner(ip: str) -> Optional[str]:
    return next((
        row["interface"] for row in _route_rows()
        if row["destination"] == ip and row["prefix_length"] == 32
    ), None)


def _change_address(add: bool, device: str, local_ip: str) -> None:
    message = _IFADDR_MESSAGE.pack(
        socket.AF_INET, 32, 0, _RT_SCOPE_UNIVERSE, socket.if_nametoindex(device),
    )
    packed_ip = socket.inet_pton(socket.AF_INET, local_ip)
    message += _attribute(_IFA_LOCAL, packed_ip) + _attribute(_IFA_ADDRESS, packed_ip)
    flags = _NLM_F_CREATE | _NLM_F_EXCL if add else 0
    try:
        _netlink_message(_RTM_NEWADDR if add else _RTM_DELADDR, flags, message)
    except CameraNetworkError as exc:
        cause = exc.__cause__
        if add and isinstance(cause, OSError) and cause.errno == errno.EEXIST:
            return
        if not add and isinstance(cause, OSError) and cause.errno == errno.EADDRNOTAVAIL:
            return
        raise


def _change_route(add: bool, device: str, local_ip: str, camera_ip: str) -> None:
    message = _ROUTE_MESSAGE.pack(
        socket.AF_INET, 32, 0, 0, _RT_TABLE_MAIN, _RTPROT_STATIC,
        _RT_SCOPE_LINK, _RTN_UNICAST, 0,
    )
    message += _attribute(_RTA_DST, socket.inet_pton(socket.AF_INET, camera_ip))
    message += _attribute(_RTA_OIF, struct.pack("=I", socket.if_nametoindex(device)))
    if add:
        message += _attribute(_RTA_PREFSRC, socket.inet_pton(socket.AF_INET, local_ip))
    flags = _NLM_F_CREATE | _NLM_F_EXCL if add else 0
    try:
        _netlink_message(_RTM_NEWROUTE if add else _RTM_DELROUTE, flags, message)
    except CameraNetworkError as exc:
        cause = exc.__cause__
        if add and isinstance(cause, OSError) and cause.errno == errno.EEXIST:
            return
        if not add and isinstance(cause, OSError) and cause.errno == errno.ESRCH:
            return
        raise


class CameraNetworkManager:
    """物理网口信息、地址选择辅助和事务可回滚的 Netlink 操作。"""

    @staticmethod
    def interfaces() -> List[Dict[str, Any]]:
        addresses = _dump_ipv4_addresses()
        routes = _route_rows()
        result: List[Dict[str, Any]] = []
        for name in _physical_ethernet_names():
            root = _SYS_NET / name
            try:
                speed = int(_read_text(root / "speed"))
                if speed < 0:
                    speed = None
            except ValueError:
                speed = None
            default = next((
                row for row in routes
                if row["interface"] == name and row["destination"] == "0.0.0.0"
                and row["prefix_length"] == 0
            ), None)
            result.append({
                "device": name,
                "ifindex": socket.if_nametoindex(name),
                "mac": _read_text(root / "address").lower(),
                "operstate": _read_text(root / "operstate") or "unknown",
                "link_up": _read_text(root / "carrier") == "1",
                "speed_mbps": speed,
                "addresses": addresses.get(name, []),
                "has_default_route": default is not None,
                "default_gateway": default["gateway"] if default else "",
            })
        return result

    @staticmethod
    def routes() -> List[Dict[str, Any]]:
        return _route_rows()

    @staticmethod
    def all_ipv4_addresses() -> Dict[str, List[str]]:
        """返回包括 Wi-Fi、虚拟网卡在内的全部 IPv4 地址，用于冲突判断。"""
        return _dump_ipv4_addresses()

    @staticmethod
    def network_state(device: str, local_ip: str, camera_ip: str) -> Dict[str, Any]:
        clean = _validate_device(device)
        local = _validate_ipv4(local_ip, "本地 IP")
        camera = _validate_ipv4(camera_ip, "摄像头 IP")
        address_owner = _address_owner(local)
        route_owner = _exact_route_owner(camera)
        return {
            "address_on_interface": address_owner == clean,
            "address_interface": address_owner,
            "route_interface": route_owner,
        }

    @staticmethod
    def apply(device: str, local_ip: str, camera_ip: str) -> Dict[str, bool]:
        clean = _validate_device(device)
        local = _validate_ipv4(local_ip, "本地 IP")
        camera = _validate_ipv4(camera_ip, "摄像头 IP")
        if local == camera:
            raise CameraNetworkError("本地 IP 不能与摄像头 IP 相同")
        state = CameraNetworkManager.network_state(clean, local, camera)
        if state["address_interface"] not in (None, clean):
            raise CameraNetworkError(f"本地候选 IP 已被网卡 {state['address_interface']} 使用")
        if state["route_interface"] not in (None, clean):
            raise CameraNetworkError(
                f"摄像头 IP 已存在指向网卡 {state['route_interface']} 的主机路由，拒绝覆盖"
            )
        address_preexisting = bool(state["address_on_interface"])
        route_preexisting = state["route_interface"] == clean
        if not address_preexisting:
            _change_address(True, clean, local)
        try:
            if not route_preexisting:
                _change_route(True, clean, local, camera)
        except Exception:
            if not address_preexisting:
                try:
                    _change_address(False, clean, local)
                except Exception:
                    pass
            raise
        return {
            "address_preexisting": address_preexisting,
            "route_preexisting": route_preexisting,
        }

    @staticmethod
    def remove(device: str, local_ip: str, camera_ip: str, *,
               remove_address: bool, remove_route: bool) -> None:
        clean = _validate_device(device)
        local = _validate_ipv4(local_ip, "本地 IP")
        camera = _validate_ipv4(camera_ip, "摄像头 IP")
        if remove_route and _exact_route_owner(camera) == clean:
            _change_route(False, clean, local, camera)
        if remove_address and _address_owner(local) == clean:
            _change_address(False, clean, local)

    @staticmethod
    def arp_probe(device: str, target_ip: str, *, source_ip: str = "0.0.0.0",
                  timeout_seconds: float = 0.3) -> Optional[str]:
        clean = _validate_device(device)
        target = _validate_ipv4(target_ip, "ARP 目标 IP")
        if source_ip == "0.0.0.0":
            source = source_ip
        else:
            source = _validate_ipv4(source_ip, "ARP 源 IP")
        timeout_seconds = max(0.05, min(float(timeout_seconds), 5.0))
        try:
            source_mac = bytes.fromhex(_read_text(_SYS_NET / clean / "address").replace(":", ""))
        except ValueError as exc:
            raise CameraNetworkError(f"无法读取网卡 {clean} 的 MAC") from exc
        if len(source_mac) != 6:
            raise CameraNetworkError(f"网卡 {clean} 的 MAC 无效")
        ethernet = struct.pack("!6s6sH", b"\xff" * 6, source_mac, _ETH_P_ARP)
        arp = struct.pack(
            "!HHBBH6s4s6s4s", _ARPHRD_ETHER, _ETH_P_IP, 6, 4, _ARPOP_REQUEST,
            source_mac, socket.inet_aton(source), b"\0" * 6, socket.inet_aton(target),
        )
        try:
            with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(_ETH_P_ARP)) as arp_socket:
                arp_socket.bind((clean, 0))
                arp_socket.send(ethernet + arp)
                deadline = time.monotonic() + timeout_seconds
                while time.monotonic() < deadline:
                    readable, _, _ = select.select(
                        [arp_socket], [], [], max(0.0, deadline - time.monotonic()),
                    )
                    if not readable:
                        break
                    response = arp_socket.recv(2048)
                    if len(response) < 42:
                        continue
                    _, _, ether_type = struct.unpack_from("!6s6sH", response)
                    if ether_type != _ETH_P_ARP:
                        continue
                    _, _, _, _, operation, sender_mac, sender_ip, _, _ = struct.unpack_from(
                        "!HHBBH6s4s6s4s", response, 14,
                    )
                    if operation == _ARPOP_REPLY and sender_ip == socket.inet_aton(target):
                        return ":".join(f"{byte:02x}" for byte in sender_mac)
        except PermissionError as exc:
            raise CameraNetworkError("ARP 探测需要 root 或 CAP_NET_RAW 权限") from exc
        except OSError as exc:
            raise CameraNetworkError(f"ARP 探测失败：{exc}") from exc
        return None
