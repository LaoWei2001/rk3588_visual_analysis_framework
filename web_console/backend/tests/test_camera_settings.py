import json
import ipaddress

import pytest

from routers.camera_settings import CameraConfigurationRequest
from services import camera_manager
from services.camera_discovery import parse_discovery_payload
from services.camera_network_manager import CameraNetworkError, CameraNetworkManager


SADP_RESPONSE = b"""<?xml version="1.0" encoding="UTF-8"?>
<ProbeMatch>
  <Types>inquiry</Types>
  <DeviceDescription>DS-2CD-Test</DeviceDescription>
  <DeviceSN>TEST123</DeviceSN>
  <HttpPort>8080</HttpPort>
  <MAC>e0-ca-3c-8d-11-a0</MAC>
  <IPv4Address>10.20.30.40</IPv4Address>
  <IPv4SubnetMask>255.255.0.0</IPv4SubnetMask>
  <IPv4Gateway>10.20.0.1</IPv4Gateway>
</ProbeMatch>"""


def _interfaces():
    return [{
        "device": "lan-camera", "ifindex": 7, "mac": "00:11:22:33:44:55",
        "operstate": "up", "link_up": True, "speed_mbps": 1000,
        "addresses": ["10.20.30.39/32"], "has_default_route": False,
        "default_gateway": "",
    }]


def _payload(**overrides):
    value = {
        "interface": "lan-camera", "camera_ip": "10.20.30.40",
        "prefix_length": 16, "camera_mac": "e0:ca:3c:8d:11:a0",
        "model": "DS-2CD-Test", "serial": "TEST123",
        "http_port": 8080, "rtsp_port": 554,
        "http_port_inferred": False, "rtsp_port_inferred": True,
    }
    value.update(overrides)
    return value


def _patch_snapshot(monkeypatch, *, routes=None, all_addresses=None, state=None):
    monkeypatch.setattr(CameraNetworkManager, "interfaces", staticmethod(_interfaces))
    monkeypatch.setattr(CameraNetworkManager, "all_ipv4_addresses", staticmethod(
        lambda: all_addresses or {
            "lan-camera": ["10.20.30.39/32"],
            "wlan0": ["10.20.50.8/24"],
            "eth0": ["172.16.1.20/24"],
        }
    ))
    monkeypatch.setattr(CameraNetworkManager, "routes", staticmethod(
        lambda: routes or [{
            "interface": "wlan0", "destination": "0.0.0.0",
            "prefix_length": 0, "gateway": "10.20.50.1", "metric": 600,
        }]
    ))
    monkeypatch.setattr(CameraNetworkManager, "network_state", staticmethod(
        lambda *_: state or {
            "address_on_interface": True,
            "address_interface": "lan-camera",
            "route_interface": None,
        }
    ))
    monkeypatch.setattr(camera_manager, "_resolver_ips", lambda: set())
    monkeypatch.setattr(camera_manager, "_arp_neighbors", lambda: [])


def test_sadp_parser_extracts_hikvision_fields_and_marks_missing_rtsp_port():
    camera = parse_discovery_payload(SADP_RESPONSE, "10.20.30.40")

    assert camera is not None
    assert camera["ip"] == "10.20.30.40"
    assert camera["mac"] == "e0:ca:3c:8d:11:a0"
    assert camera["model"] == "DS-2CD-Test"
    assert camera["prefix_length"] == 16
    assert camera["http_port"] == 8080
    assert camera["http_port_inferred"] is False
    assert camera["rtsp_port"] == 554
    assert camera["rtsp_port_inferred"] is True
    assert camera["hikvision"] is True


def test_plan_uses_existing_camera_side_ip_and_host_route_for_overlap(monkeypatch):
    _patch_snapshot(monkeypatch)

    plan = camera_manager.plan_configuration(_payload(), "172.16.1.99")

    assert plan["local_ip"] == "10.20.30.39"
    assert plan["local_prefix_length"] == 32
    assert plan["camera_network"] == "10.20.0.0/16"
    assert plan["isolation_mode"] == "host-route"
    assert plan["conflicts"] == [{
        "interface": "wlan0", "network": "10.20.50.0/24",
        "address": "10.20.50.8/24",
    }]
    assert any("/32 主机路由" in warning for warning in plan["warnings"])
    assert "gateway" not in plan


def test_plan_rejects_camera_ip_equal_to_default_gateway(monkeypatch):
    _patch_snapshot(monkeypatch, all_addresses={
        "lan-camera": [], "wlan0": ["10.20.50.8/24"],
    })

    with pytest.raises(camera_manager.CameraManagerError, match="默认网关"):
        camera_manager.plan_configuration(
            _payload(camera_ip="10.20.50.1", prefix_length=24), "172.16.1.99",
        )


def test_plan_rejects_exact_management_client_collision(monkeypatch):
    _patch_snapshot(monkeypatch, all_addresses={"lan-camera": []})

    with pytest.raises(camera_manager.CameraManagerError, match="Web 管理客户端"):
        camera_manager.plan_configuration(
            _payload(camera_ip="10.20.30.88"), "10.20.30.88",
        )


def test_plan_rejects_camera_ip_used_by_active_server_connection(monkeypatch):
    _patch_snapshot(monkeypatch, all_addresses={"lan-camera": []})
    monkeypatch.setattr(camera_manager, "_active_remote_ipv4s", lambda: {
        ipaddress.IPv4Address("10.20.30.88"),
    })

    with pytest.raises(camera_manager.CameraManagerError, match="服务器连接"):
        camera_manager.plan_configuration(
            _payload(camera_ip="10.20.30.88"), "172.16.1.99",
        )


def test_camera_request_is_data_only_and_rejects_shell_device_name():
    with pytest.raises(Exception):
        CameraConfigurationRequest(
            **_payload(interface="eth1; reboot"),
        )


def test_failed_apply_restores_owned_old_resources(tmp_path, monkeypatch):
    config_file = tmp_path / "camera.json"
    lock_file = tmp_path / "camera.lock"
    monkeypatch.setattr(camera_manager, "CONFIG_FILE", config_file)
    monkeypatch.setattr(camera_manager, "LOCK_FILE", lock_file)
    monkeypatch.setattr(camera_manager, "TRANSACTION_FILE", tmp_path / "camera-transaction.json")
    old = {
        "version": 1, "interface": "lan-camera", "camera_ip": "10.20.30.40",
        "camera_prefix_length": 16, "camera_network": "10.20.0.0/16",
        "local_ip": "10.20.30.39", "local_prefix_length": 32,
        "address_owned": True, "route_owned": True,
        "http_port": 80, "rtsp_port": 554,
    }
    config_file.write_text(json.dumps(old), encoding="utf-8")
    new_plan = {
        **_payload(camera_ip="192.0.2.10", prefix_length=24),
        "camera_prefix_length": 24, "camera_network": "192.0.2.0/24",
        "local_ip": "192.0.2.11", "local_prefix_length": 32,
        "isolation_mode": "host-route", "conflicts": [], "warnings": [],
        "address_preexisting": False, "route_preexisting": False,
    }
    new_plan.pop("prefix_length")
    monkeypatch.setattr(camera_manager, "plan_configuration", lambda *_, **__: new_plan)
    calls = []

    def fake_remove(device, local_ip, camera_ip, **ownership):
        calls.append(("remove", device, local_ip, camera_ip, ownership))

    def fake_apply(device, local_ip, camera_ip):
        calls.append(("apply", device, local_ip, camera_ip))
        if camera_ip == "192.0.2.10":
            raise CameraNetworkError("simulated netlink failure")
        return {"address_preexisting": False, "route_preexisting": False}

    monkeypatch.setattr(CameraNetworkManager, "remove", staticmethod(fake_remove))
    monkeypatch.setattr(CameraNetworkManager, "apply", staticmethod(fake_apply))
    monkeypatch.setattr(CameraNetworkManager, "interfaces", staticmethod(_interfaces))

    with pytest.raises(camera_manager.CameraManagerError, match="simulated netlink failure"):
        camera_manager.apply_configuration(_payload())

    assert calls[0][0] == "remove"
    assert calls[-1] == ("apply", "lan-camera", "10.20.30.39", "10.20.30.40")
    assert json.loads(config_file.read_text(encoding="utf-8"))["camera_ip"] == "10.20.30.40"


def test_successful_apply_does_not_claim_preexisting_address_or_route(tmp_path, monkeypatch):
    monkeypatch.setattr(camera_manager, "CONFIG_FILE", tmp_path / "camera.json")
    monkeypatch.setattr(camera_manager, "LOCK_FILE", tmp_path / "camera.lock")
    monkeypatch.setattr(camera_manager, "TRANSACTION_FILE", tmp_path / "camera-transaction.json")
    plan = {
        **_payload(), "camera_prefix_length": 16, "camera_network": "10.20.0.0/16",
        "local_ip": "10.20.30.39", "local_prefix_length": 32,
        "isolation_mode": "host-route", "conflicts": [], "warnings": [],
        "address_preexisting": True, "route_preexisting": True,
    }
    plan.pop("prefix_length")
    monkeypatch.setattr(camera_manager, "plan_configuration", lambda *_, **__: plan)
    monkeypatch.setattr(CameraNetworkManager, "apply", staticmethod(
        lambda *_: {"address_preexisting": True, "route_preexisting": True}
    ))
    monkeypatch.setattr(CameraNetworkManager, "network_state", staticmethod(
        lambda *_: {
            "address_on_interface": True, "address_interface": "lan-camera",
            "route_interface": "lan-camera",
        }
    ))

    saved = camera_manager.apply_configuration(_payload())

    assert saved["address_owned"] is False
    assert saved["route_owned"] is False
    assert camera_manager.read_configuration()["camera_ip"] == "10.20.30.40"


def test_pending_transaction_recovery_removes_new_and_restores_old(tmp_path, monkeypatch):
    transaction_file = tmp_path / "camera-transaction.json"
    monkeypatch.setattr(camera_manager, "TRANSACTION_FILE", transaction_file)
    monkeypatch.setattr(camera_manager, "CONFIG_FILE", tmp_path / "camera.json")
    monkeypatch.setattr(camera_manager, "LOCK_FILE", tmp_path / "camera.lock")
    old = {
        "version": 1, "interface": "lan-camera", "camera_ip": "10.20.30.40",
        "camera_prefix_length": 16, "local_ip": "10.20.30.39",
        "address_owned": True, "route_owned": True,
    }
    new = {
        "interface": "lan-camera", "camera_ip": "192.0.2.10",
        "local_ip": "192.0.2.11", "address_owned": True, "route_owned": True,
    }
    transaction_file.write_text(json.dumps({
        "id": "deadbeef", "created_at": 1, "old": old, "new": new,
    }), encoding="utf-8")
    calls = []
    monkeypatch.setattr(CameraNetworkManager, "interfaces", staticmethod(_interfaces))
    monkeypatch.setattr(CameraNetworkManager, "remove", staticmethod(
        lambda device, local, camera, **flags: calls.append(("remove", device, local, camera, flags))
    ))
    monkeypatch.setattr(CameraNetworkManager, "apply", staticmethod(
        lambda device, local, camera: calls.append(("apply", device, local, camera)) or {
            "address_preexisting": False, "route_preexisting": False,
        }
    ))

    with camera_manager._config_lock():
        camera_manager._recover_pending_transaction_unlocked()

    assert calls[0][0:4] == ("remove", "lan-camera", "192.0.2.11", "192.0.2.10")
    assert calls[1] == ("apply", "lan-camera", "10.20.30.39", "10.20.30.40")
    assert not transaction_file.exists()
