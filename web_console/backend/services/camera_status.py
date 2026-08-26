"""摄像头物理链路、ARP/IP、HTTP 与 RTSP 分层状态检测。"""
from __future__ import annotations

import ipaddress
import re
import socket
import time
from typing import Any, Dict, Tuple

from .camera_network_manager import CameraNetworkError, CameraNetworkManager


SO_BINDTODEVICE = getattr(socket, "SO_BINDTODEVICE", 25)
_STATUS_RE = re.compile(rb"^(?:HTTP|RTSP)/\d\.\d\s+(\d{3})", re.IGNORECASE)


class CameraStatusError(RuntimeError):
    pass


def _validate_port(value: int, label: str) -> int:
    try:
        port = int(value)
    except (TypeError, ValueError) as exc:
        raise CameraStatusError(f"{label}不是有效端口") from exc
    if not 1 <= port <= 65535:
        raise CameraStatusError(f"{label}必须在 1 到 65535 之间")
    return port


def _connect(device: str, local_ip: str, remote_ip: str,
             port: int, timeout_seconds: float) -> socket.socket:
    connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        connection.setsockopt(socket.SOL_SOCKET, SO_BINDTODEVICE, device.encode() + b"\0")
        connection.settimeout(timeout_seconds)
        if local_ip:
            connection.bind((local_ip, 0))
        connection.connect((remote_ip, port))
        return connection
    except Exception:
        connection.close()
        raise


def _protocol_probe(device: str, local_ip: str, camera_ip: str,
                    port: int, timeout_seconds: float, *, rtsp: bool) -> Tuple[bool, int]:
    try:
        with _connect(device, local_ip, camera_ip, port, timeout_seconds) as connection:
            if rtsp:
                request = (
                    f"OPTIONS rtsp://{camera_ip}:{port}/ RTSP/1.0\r\n"
                    "CSeq: 1\r\nUser-Agent: rk3588-camera-manager\r\n\r\n"
                ).encode("ascii")
            else:
                request = (
                    f"GET / HTTP/1.0\r\nHost: {camera_ip}\r\n"
                    "User-Agent: rk3588-camera-manager\r\nConnection: close\r\n\r\n"
                ).encode("ascii")
            connection.sendall(request)
            response = connection.recv(2048)
    except (OSError, TimeoutError):
        return False, 0
    match = _STATUS_RE.match(response)
    return (True, int(match.group(1))) if match else (False, 0)


class CameraStatus:
    @staticmethod
    def check(device: str, local_ip: str, camera_ip: str,
              http_port: int, rtsp_port: int,
              *, timeout_seconds: float = 0.8) -> Dict[str, Any]:
        try:
            camera = str(ipaddress.IPv4Address(camera_ip))
            local = str(ipaddress.IPv4Address(local_ip)) if local_ip else ""
        except ValueError as exc:
            raise CameraStatusError("状态检测 IP 地址无效") from exc
        http = _validate_port(http_port, "HTTP 端口")
        rtsp = _validate_port(rtsp_port, "RTSP 端口")
        timeout_seconds = max(0.1, min(float(timeout_seconds), 5.0))
        try:
            interface = next(
                (item for item in CameraNetworkManager.interfaces() if item["device"] == device),
                None,
            )
        except CameraNetworkError as exc:
            raise CameraStatusError(str(exc)) from exc
        if interface is None:
            raise CameraStatusError(f"物理有线网口 {device} 不存在")

        result: Dict[str, Any] = {
            "checked_at": time.time(),
            "link_up": bool(interface["link_up"]),
            "arp_reachable": False,
            "arp_mac": "",
            "http_reachable": False,
            "http_status": 0,
            "rtsp_reachable": False,
            "rtsp_status": 0,
        }
        if not result["link_up"]:
            return result
        try:
            arp_mac = CameraNetworkManager.arp_probe(
                device, camera, source_ip=local or "0.0.0.0",
                timeout_seconds=timeout_seconds,
            )
        except CameraNetworkError as exc:
            raise CameraStatusError(str(exc)) from exc
        result["arp_reachable"] = arp_mac is not None
        result["arp_mac"] = arp_mac or ""
        if arp_mac is None:
            return result
        http_result = _protocol_probe(
            device, local, camera, http, timeout_seconds, rtsp=False,
        )
        rtsp_result = _protocol_probe(
            device, local, camera, rtsp, timeout_seconds, rtsp=True,
        )
        result.update({
            "http_reachable": http_result[0],
            "http_status": http_result[1],
            "rtsp_reachable": rtsp_result[0],
            "rtsp_status": rtsp_result[1],
        })
        return result
