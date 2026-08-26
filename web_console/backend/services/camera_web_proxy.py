"""按需、限时的摄像头 Web TCP 代理。

浏览器连接 RK3588 的代理端口，代理再用摄像头侧本地 IP 并绑定指定网口连接摄像
头。TCP 透明转发不会破坏 Hikvision Digest 认证，也无需开启系统 IP 转发或 NAT。
"""
from __future__ import annotations

import ipaddress
import os
import select
import socket
import threading
import time
from typing import Any, Dict, Optional, Set, Tuple

from . import camera_manager


SO_BINDTODEVICE = getattr(socket, "SO_BINDTODEVICE", 25)
DEFAULT_PROXY_PORT = 18080
DEFAULT_SESSION_SECONDS = 30 * 60
DEFAULT_IDLE_SECONDS = 5 * 60


class CameraWebProxyError(RuntimeError):
    pass


def _environment_port() -> int:
    raw = os.environ.get("CAMERA_WEB_PROXY_PORT", str(DEFAULT_PROXY_PORT)).strip()
    try:
        port = int(raw)
    except ValueError as exc:
        raise CameraWebProxyError("CAMERA_WEB_PROXY_PORT 必须是 0 到 65535 的整数") from exc
    if not 0 <= port <= 65535:
        raise CameraWebProxyError("CAMERA_WEB_PROXY_PORT 必须是 0 到 65535 的整数")
    return port


def _normalize_client_ip(value: str) -> str:
    try:
        address = ipaddress.ip_address(str(value).strip())
    except ValueError as exc:
        raise CameraWebProxyError("无法确认当前管理客户端 IP") from exc
    if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped:
        return str(address.ipv4_mapped)
    if not isinstance(address, ipaddress.IPv4Address):
        raise CameraWebProxyError("当前摄像头 Web 代理仅支持 IPv4 管理客户端")
    return str(address)


def _validated_target(config: Dict[str, Any]) -> Tuple[str, str, str, int]:
    """只接受 manager 写入的基础网络字段，不把持久化文本直接交给 socket。"""
    interface = str(config.get("interface", "")).strip()
    if not interface or len(interface.encode("utf-8")) > 15:
        raise CameraWebProxyError("持久化配置中的摄像头网口名称无效")
    try:
        interface.encode("ascii")
        socket.if_nametoindex(interface)
    except (OSError, UnicodeEncodeError) as exc:
        raise CameraWebProxyError(f"持久化配置中的摄像头网口 {interface} 不存在") from exc
    try:
        local_ip = str(ipaddress.IPv4Address(str(config.get("local_ip", "")).strip()))
        camera_ip = str(ipaddress.IPv4Address(str(config.get("camera_ip", "")).strip()))
    except ValueError as exc:
        raise CameraWebProxyError("持久化配置中的本地 IP 或摄像头 IP 无效") from exc
    try:
        http_port = int(config.get("http_port", 80))
    except (TypeError, ValueError) as exc:
        raise CameraWebProxyError("持久化配置中的摄像头 HTTP 端口无效") from exc
    if not 1 <= http_port <= 65535:
        raise CameraWebProxyError("持久化配置中的摄像头 HTTP 端口无效")
    return interface, local_ip, camera_ip, http_port


class CameraWebProxy:
    def __init__(self, *, bind_host: str = "0.0.0.0", port: Optional[int] = None,
                 session_seconds: int = DEFAULT_SESSION_SECONDS,
                 idle_seconds: int = DEFAULT_IDLE_SECONDS) -> None:
        try:
            ipaddress.IPv4Address(bind_host)
        except ValueError as exc:
            raise CameraWebProxyError("摄像头 Web 代理绑定地址必须是 IPv4") from exc
        self._bind_host = bind_host
        self._requested_port = None if port is None else int(port)
        if self._requested_port is not None and not 0 <= self._requested_port <= 65535:
            raise CameraWebProxyError("摄像头 Web 代理端口超出范围")
        self._session_seconds = max(60, min(int(session_seconds), 12 * 60 * 60))
        self._idle_seconds = max(30, min(int(idle_seconds), 60 * 60))
        self._lock = threading.RLock()
        self._stop_event = threading.Event()
        self._listener: Optional[socket.socket] = None
        self._listener_thread: Optional[threading.Thread] = None
        self._sessions: Dict[str, float] = {}
        self._active_sockets: Set[socket.socket] = set()
        self._port = 0
        self._fallback_port = False

    @property
    def port(self) -> int:
        with self._lock:
            return self._port

    def start_session(self, client_ip: str) -> Dict[str, Any]:
        allowed_ip = _normalize_client_ip(client_ip)
        config = camera_manager.read_configuration()
        if not config:
            raise CameraWebProxyError("请先应用摄像头网络配置")
        if not config.get("interface") or not config.get("local_ip") or not config.get("camera_ip"):
            raise CameraWebProxyError("摄像头网络配置不完整")
        _validated_target(config)
        expires_at = time.time() + self._session_seconds
        with self._lock:
            self._ensure_listener_locked()
            self._sessions[allowed_ip] = expires_at
            return {
                "port": self._port,
                "expires_at": expires_at,
                "allowed_client_ip": allowed_ip,
                "fallback_port": self._fallback_port,
            }

    def _ensure_listener_locked(self) -> None:
        if self._listener is not None:
            return
        if self._stop_event.is_set():
            raise CameraWebProxyError("摄像头 Web 代理正在停止")
        requested_port = _environment_port() if self._requested_port is None else self._requested_port
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            listener.bind((self._bind_host, requested_port))
        except OSError as preferred_error:
            if requested_port == 0:
                listener.close()
                raise CameraWebProxyError(f"启动摄像头 Web 代理失败：{preferred_error}") from preferred_error
            listener.close()
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                listener.bind((self._bind_host, 0))
                self._fallback_port = True
            except OSError as fallback_error:
                listener.close()
                raise CameraWebProxyError(
                    f"代理端口 {requested_port} 及动态备用端口均无法绑定：{fallback_error}"
                ) from fallback_error
        try:
            listener.listen(32)
            listener.settimeout(1.0)
        except OSError as exc:
            listener.close()
            raise CameraWebProxyError(f"启动摄像头 Web 代理失败：{exc}") from exc
        self._listener = listener
        self._port = int(listener.getsockname()[1])
        self._listener_thread = threading.Thread(
            target=self._accept_loop, name="camera-web-proxy", daemon=True,
        )
        self._listener_thread.start()

    def _accept_loop(self) -> None:
        while not self._stop_event.is_set():
            with self._lock:
                listener = self._listener
                now = time.time()
                self._sessions = {
                    address: deadline for address, deadline in self._sessions.items()
                    if deadline > now
                }
            if listener is None:
                return
            try:
                client, address = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            client_ip = address[0]
            with self._lock:
                authorized = self._sessions.get(client_ip, 0) > time.time()
            if not authorized:
                client.close()
                continue
            threading.Thread(
                target=self._handle_client, args=(client,),
                name=f"camera-web-{client_ip}", daemon=True,
            ).start()

    def _open_upstream(self, config: Dict[str, Any]) -> socket.socket:
        interface, local_ip, camera_ip, http_port = _validated_target(config)
        upstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            upstream.setsockopt(
                socket.SOL_SOCKET, SO_BINDTODEVICE,
                interface.encode("ascii") + b"\0",
            )
            upstream.settimeout(5.0)
            upstream.bind((local_ip, 0))
            upstream.connect((camera_ip, http_port))
            upstream.settimeout(None)
            return upstream
        except Exception:
            upstream.close()
            raise

    def _handle_client(self, client: socket.socket) -> None:
        upstream: Optional[socket.socket] = None
        try:
            config = camera_manager.read_configuration()
            if not config:
                return
            upstream = self._open_upstream(config)
            with self._lock:
                self._active_sockets.add(client)
                self._active_sockets.add(upstream)
            self._relay(client, upstream)
        except (OSError, CameraWebProxyError, camera_manager.CameraManagerError):
            # 浏览器会显示连接失败；这里不能返回伪 HTTP 响应，否则会破坏透明代理。
            return
        finally:
            with self._lock:
                self._active_sockets.discard(client)
                if upstream is not None:
                    self._active_sockets.discard(upstream)
            for connection in (client, upstream):
                if connection is not None:
                    try:
                        connection.close()
                    except OSError:
                        pass

    def _relay(self, client: socket.socket, upstream: socket.socket) -> None:
        readable: Set[socket.socket] = {client, upstream}
        peers = {client: upstream, upstream: client}
        last_activity = time.monotonic()
        while readable and not self._stop_event.is_set():
            try:
                ready, _, failed = select.select(list(readable), [], list(readable), 1.0)
            except (OSError, ValueError):
                return
            if failed:
                return
            if not ready:
                if time.monotonic() - last_activity >= self._idle_seconds:
                    return
                continue
            for source in ready:
                destination = peers[source]
                try:
                    data = source.recv(65536)
                except OSError:
                    return
                if not data:
                    readable.discard(source)
                    try:
                        destination.shutdown(socket.SHUT_WR)
                    except OSError:
                        pass
                    continue
                last_activity = time.monotonic()
                try:
                    destination.sendall(data)
                except OSError:
                    return

    def stop(self) -> None:
        self._stop_event.set()
        with self._lock:
            listener = self._listener
            self._listener = None
            active = list(self._active_sockets)
            self._active_sockets.clear()
            self._sessions.clear()
        if listener is not None:
            try:
                listener.close()
            except OSError:
                pass
        for connection in active:
            try:
                connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                connection.close()
            except OSError:
                pass
        thread = self._listener_thread
        if thread is not None and thread.is_alive():
            thread.join(timeout=2.0)


camera_web_proxy = CameraWebProxy()
