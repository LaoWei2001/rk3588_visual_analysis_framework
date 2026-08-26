import socket
import threading

import pytest

from services import camera_web_proxy as proxy_module
from services.camera_web_proxy import CameraWebProxy, CameraWebProxyError


def _configuration():
    return {
        "interface": "lo",
        "local_ip": "127.0.0.1",
        "camera_ip": "127.0.0.2",
        "http_port": 80,
    }


def test_proxy_requires_persisted_camera_configuration(monkeypatch):
    proxy = CameraWebProxy(bind_host="127.0.0.1", port=0)
    monkeypatch.setattr(proxy_module.camera_manager, "read_configuration", lambda: None)

    with pytest.raises(CameraWebProxyError, match="先应用"):
        proxy.start_session("127.0.0.1")

    assert proxy.port == 0


def test_proxy_relays_tcp_for_authorized_management_client(monkeypatch):
    echo_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    echo_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    echo_server.bind(("127.0.0.1", 0))
    echo_server.listen(1)
    echo_server.settimeout(3.0)

    def echo_once():
        try:
            connection, _ = echo_server.accept()
            with connection:
                while True:
                    data = connection.recv(65536)
                    if not data:
                        return
                    connection.sendall(data)
        except OSError:
            return

    echo_thread = threading.Thread(target=echo_once, daemon=True)
    echo_thread.start()

    proxy = CameraWebProxy(bind_host="127.0.0.1", port=0)
    monkeypatch.setattr(proxy_module.camera_manager, "read_configuration", _configuration)
    monkeypatch.setattr(
        proxy, "_open_upstream",
        lambda _config: socket.create_connection(echo_server.getsockname(), timeout=2.0),
    )

    try:
        session = proxy.start_session("127.0.0.1")
        assert session["port"] > 0
        assert session["allowed_client_ip"] == "127.0.0.1"

        with socket.create_connection(("127.0.0.1", session["port"]), timeout=2.0) as client:
            client.sendall(b"GET /doc/page/login.asp HTTP/1.1\r\n\r\n")
            assert client.recv(65536) == b"GET /doc/page/login.asp HTTP/1.1\r\n\r\n"
    finally:
        proxy.stop()
        echo_server.close()
        echo_thread.join(timeout=2.0)


def test_proxy_rejects_non_ipv4_management_client(monkeypatch):
    proxy = CameraWebProxy(bind_host="127.0.0.1", port=0)
    monkeypatch.setattr(proxy_module.camera_manager, "read_configuration", _configuration)

    with pytest.raises(CameraWebProxyError, match="仅支持 IPv4"):
        proxy.start_session("2001:db8::10")
