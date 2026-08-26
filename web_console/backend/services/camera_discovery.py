"""绑定指定有线网口的海康 SADP / ONVIF WS-Discovery 发现。

本模块只负责二层/UDP 发现与报文解析，不读取或修改主机网络配置。SADP 固件未
广播的服务端口会带 ``*_port_inferred`` 标记，交由页面明确提示并允许人工覆盖。
"""
from __future__ import annotations

import html
import ipaddress
import re
import select
import socket
import struct
import time
import uuid
import xml.etree.ElementTree as ET
from typing import Any, Dict, Iterable, List
from urllib.parse import unquote, urlsplit


DISCOVERY_GROUP = "239.255.255.250"
DISCOVERY_PORT = 37020
SO_BINDTODEVICE = getattr(socket, "SO_BINDTODEVICE", 25)
_DEVICE_RE = re.compile(r"^[A-Za-z0-9_.:-]{1,15}$")
_MAC_RE = re.compile(r"^[0-9a-f]{12}$")


class CameraDiscoveryError(RuntimeError):
    pass


def _validate_device(device: str) -> str:
    clean = str(device).strip()
    if not _DEVICE_RE.fullmatch(clean):
        raise CameraDiscoveryError("非法的网卡名称")
    try:
        socket.if_nametoindex(clean)
    except OSError as exc:
        raise CameraDiscoveryError(f"网卡 {clean} 不存在") from exc
    return clean


def _normalize_mac(value: str) -> str:
    compact = "".join(char.lower() for char in value if char.lower() in "0123456789abcdef")
    if not _MAC_RE.fullmatch(compact):
        return value.strip().lower()
    return ":".join(compact[index:index + 2] for index in range(0, 12, 2))


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1].rsplit(":", 1)[-1].lower()


def _xml_values(payload: str) -> Dict[str, List[str]]:
    values: Dict[str, List[str]] = {}
    try:
        root = ET.fromstring(payload)
    except ET.ParseError:
        # 个别旧固件会在 XML 末尾附加 NUL；清理后再尝试一次。
        try:
            root = ET.fromstring(payload.rstrip("\x00\r\n "))
        except ET.ParseError:
            return values
    for element in root.iter():
        text = html.unescape((element.text or "").strip())
        if text:
            values.setdefault(_local_name(element.tag), []).append(text)
    return values


def _first(values: Dict[str, List[str]], names: Iterable[str]) -> str:
    for name in names:
        items = values.get(name.lower())
        if items:
            return items[0]
    return ""


def _port(value: str) -> int | None:
    try:
        port = int(value)
    except (TypeError, ValueError):
        return None
    return port if 1 <= port <= 65535 else None


def _prefix_from_mask(mask: str) -> int | None:
    if not mask:
        return None
    try:
        return ipaddress.IPv4Network(f"0.0.0.0/{mask}").prefixlen
    except (ValueError, ipaddress.NetmaskValueError):
        return None


def _onvif_model(scopes: str) -> str:
    marker = "onvif://www.onvif.org/hardware/"
    for scope in scopes.split():
        if scope.lower().startswith(marker):
            return unquote(scope[len(marker):]).strip()
    return ""


def parse_discovery_payload(payload: bytes | str, sender_ip: str = "") -> Dict[str, Any] | None:
    """解析单个 SADP/WS-Discovery 响应；无法识别时返回 ``None``。"""
    if isinstance(payload, bytes):
        text = payload.decode("utf-8", errors="replace")
    else:
        text = payload
    values = _xml_values(text)
    if not values:
        return None
    lower_text = text.lower()
    is_response = any(key in values for key in (
        "ipv4address", "devicedescription", "probematch", "xaddrs",
    )) or "probematch" in lower_text
    if not is_response:
        return None

    camera_ip = _first(values, ("ipv4address", "ipaddress", "ipv4"))
    http_port = _port(_first(values, ("httpport",)))
    xaddrs = _first(values, ("xaddrs", "xaddr"))
    if xaddrs:
        for item in xaddrs.split():
            try:
                parsed = urlsplit(item)
                candidate = ipaddress.IPv4Address(parsed.hostname or "")
            except ValueError:
                continue
            camera_ip = str(candidate)
            if http_port is None and parsed.port:
                http_port = parsed.port
            break
    if not camera_ip:
        camera_ip = sender_ip
    try:
        camera_ip = str(ipaddress.IPv4Address(camera_ip))
    except ValueError:
        return None

    model = _first(values, (
        "devicedescription", "devicemodel", "model", "devicetype",
    ))
    scopes = " ".join(values.get("scopes", []))
    if not model:
        model = _onvif_model(scopes)
    subnet_mask = _first(values, ("ipv4subnetmask", "subnetmask"))
    rtsp_port = _port(_first(values, ("rtspport",)))
    result = {
        "ip": camera_ip,
        "mac": _normalize_mac(_first(values, ("mac", "macaddress"))),
        "model": model,
        "serial": _first(values, ("devicesn", "serialnumber")),
        "subnet_mask": subnet_mask,
        "prefix_length": _prefix_from_mask(subnet_mask),
        "gateway": _first(values, ("ipv4gateway", "gateway", "defaultgateway")),
        "http_port": http_port or 80,
        "rtsp_port": rtsp_port or 554,
        "http_port_inferred": http_port is None,
        "rtsp_port_inferred": rtsp_port is None,
        "hikvision": (
            "hikvision" in lower_text
            or "devicedescription" in values
            or _first(values, ("types",)).lower() == "inquiry"
        ),
        "source": "onvif" if "xaddrs" in values else "sadp",
    }
    return result


def _merge_camera(target: Dict[str, Any], incoming: Dict[str, Any]) -> None:
    for key in ("ip", "mac", "model", "serial", "subnet_mask", "prefix_length", "gateway"):
        if target.get(key) in (None, "") and incoming.get(key) not in (None, ""):
            target[key] = incoming[key]
    for port_name in ("http", "rtsp"):
        port = f"{port_name}_port"
        inferred = f"{port_name}_port_inferred"
        if target.get(port) is None or (target.get(inferred) and not incoming.get(inferred)):
            target[port] = incoming.get(port)
            target[inferred] = incoming.get(inferred, False)
    target["hikvision"] = bool(target.get("hikvision") or incoming.get("hikvision"))
    sources = set(str(target.get("source", "")).split("+")) | set(str(incoming.get("source", "")).split("+"))
    target["source"] = "+".join(sorted(item for item in sources if item))


class CameraDiscovery:
    """在一个明确指定的物理网口上搜索摄像头。"""

    @staticmethod
    def discover(device: str, timeout_seconds: float = 3.0) -> List[Dict[str, Any]]:
        clean = _validate_device(device)
        timeout_seconds = max(0.2, min(float(timeout_seconds), 15.0))
        interface_index = socket.if_nametoindex(clean)
        probe_id = str(uuid.uuid4())
        sadp = (
            '<?xml version="1.0" encoding="utf-8"?>'
            f"<Probe><Uuid>{probe_id}</Uuid><Types>inquiry</Types></Probe>"
        ).encode()
        onvif = (
            '<?xml version="1.0" encoding="UTF-8"?>'
            '<e:Envelope xmlns:e="http://www.w3.org/2003/05/soap-envelope" '
            'xmlns:w="http://schemas.xmlsoap.org/ws/2004/08/addressing" '
            'xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery" '
            'xmlns:dn="http://www.onvif.org/ver10/network/wsdl">'
            f"<e:Header><w:MessageID>uuid:{probe_id}</w:MessageID>"
            '<w:To e:mustUnderstand="true">urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>'
            '<w:Action e:mustUnderstand="true">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>'
            '</e:Header><e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types>'
            '</d:Probe></e:Body></e:Envelope>'
        ).encode()

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as discovery_socket:
            discovery_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, "SO_REUSEPORT"):
                try:
                    discovery_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
                except OSError:
                    pass
            discovery_socket.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            try:
                discovery_socket.setsockopt(
                    socket.SOL_SOCKET, SO_BINDTODEVICE, clean.encode() + b"\0",
                )
            except OSError as exc:
                raise CameraDiscoveryError(f"无法绑定网卡 {clean}：{exc}") from exc
            try:
                discovery_socket.bind(("0.0.0.0", DISCOVERY_PORT))
            except OSError:
                discovery_socket.bind(("0.0.0.0", 0))
            membership = struct.pack(
                "=4s4sI", socket.inet_aton(DISCOVERY_GROUP), b"\0" * 4, interface_index,
            )
            try:
                discovery_socket.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
                discovery_socket.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, membership)
            except OSError:
                # 没有 IPv4 的新网口仍可通过定向广播发 SADP，组播失败不应阻止搜索。
                pass

            sent = 0
            for payload, destination in (
                (sadp, (DISCOVERY_GROUP, DISCOVERY_PORT)),
                (sadp, ("255.255.255.255", DISCOVERY_PORT)),
                (onvif, (DISCOVERY_GROUP, DISCOVERY_PORT)),
            ):
                try:
                    discovery_socket.sendto(payload, destination)
                    sent += 1
                except OSError:
                    continue
            if sent == 0:
                raise CameraDiscoveryError(f"无法从网卡 {clean} 发送发现报文，请检查物理 Link 和权限")

            cameras: Dict[str, Dict[str, Any]] = {}
            deadline = time.monotonic() + timeout_seconds
            while time.monotonic() < deadline:
                readable, _, _ = select.select(
                    [discovery_socket], [], [], max(0.0, deadline - time.monotonic()),
                )
                if not readable:
                    break
                try:
                    payload, sender = discovery_socket.recvfrom(65535)
                except OSError:
                    continue
                camera = parse_discovery_payload(payload, sender[0])
                if camera is None:
                    continue
                key = camera.get("mac") or camera["ip"]
                existing_key = next((
                    current_key for current_key, current in cameras.items()
                    if current.get("ip") == camera.get("ip")
                    or (
                        current.get("mac") and camera.get("mac")
                        and current.get("mac") == camera.get("mac")
                    )
                ), None)
                if existing_key is not None:
                    _merge_camera(cameras[existing_key], camera)
                else:
                    cameras[key] = camera
            return sorted(cameras.values(), key=lambda item: ipaddress.IPv4Address(item["ip"]))
