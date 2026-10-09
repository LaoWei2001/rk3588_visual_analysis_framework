from __future__ import annotations

from typing import Literal, Optional

from fastapi import APIRouter, HTTPException, Response
from pydantic import BaseModel, ConfigDict, Field

from services import receiver_package as package_service
from services.dataset_manager import dataset_manager as manager

router = APIRouter()

# This feature owns its public transfer policy; new routes require console login.
PUBLIC_RECEIVER_REQUESTS = frozenset({
    ('POST', '/api/dataset-receiver/poll'),
    ('GET', '/api/dataset-receiver/image'),
    ('POST', '/api/dataset-receiver/ack'),
    ('POST', '/api/dataset-receiver/disconnect'),
})


def is_public_receiver_request(method: str, path: str) -> bool:
    return (method, path) in PUBLIC_RECEIVER_REQUESTS


class TaskConfig(BaseModel):
    model_config = ConfigDict(extra='forbid')
    rules: list[dict] = Field(min_length=1, max_length=32)
    interval_sec: float = Field(default=2, ge=.5, le=86400, allow_inf_nan=False)
    confirm_sec: float = Field(default=0, ge=0, le=60, allow_inf_nan=False)
    max_samples: int = Field(default=0, ge=0, le=1000000000)
    images_per_folder: int = Field(default=30, ge=1, le=10000)
    duration_hours: float = Field(default=0, ge=0, le=8760, allow_inf_nan=False)
    jpeg_quality: int = Field(default=95, ge=60, le=100)
    roi_name: str = Field(default='', max_length=100)
    roi_anchor: str = Field(default='center', pattern='^(center|foot)$')
    trigger_mode: str = Field(default='periodic', pattern='^(periodic|on_enter)$')


class Enabled(BaseModel):
    enabled: bool


class ReceiverPackage(BaseModel):
    url: str = Field(default='', max_length=1000)
    platform: Literal['windows-amd64', 'python'] = 'python'


class ReceiverIdentity(BaseModel):
    client: str = Field(min_length=1, max_length=100)


class InventoryCount(BaseModel):
    app: str = Field(min_length=1, max_length=120, pattern=r'^[A-Za-z0-9_+\-][A-Za-z0-9_.+\-]*$')
    channel_id: int = Field(ge=0, lt=32, strict=True)
    count: int = Field(ge=0, le=1000000000, strict=True)


class ReceiverInventory(BaseModel):
    scan_id: str = Field(min_length=1, max_length=64)
    counts: Optional[list[InventoryCount]] = Field(default=None, max_length=1024)
    error: str = Field(default='', max_length=500)


class Heartbeat(ReceiverIdentity):
    directory: str = Field(default='', max_length=500)
    error: str = Field(default='', max_length=500)
    inventory: Optional[ReceiverInventory] = None


class Ack(BaseModel):
    app: str
    task_id: str = Field(max_length=80)
    revision: str = Field(max_length=80)
    sample_id: str = Field(max_length=160)
    proof: str = Field(max_length=64)


@router.get('/apps/{name}/dataset')
def list_tasks(name: str):
    return manager.list(name)


@router.put('/apps/{name}/dataset/channels/{channel}')
def save_task(name: str, channel: int, config: TaskConfig):
    return manager.save(name, channel, config.model_dump())


@router.post('/apps/{name}/dataset/tasks/{task_id}/enabled')
def enable_task(name: str, task_id: str, body: Enabled):
    return manager.set_enabled(name, task_id, body.enabled)


@router.delete('/apps/{name}/dataset/tasks/{task_id}')
def delete_task(name: str, task_id: str):
    return manager.remove(name, task_id)


@router.post('/dataset/receiver-package')
def receiver_package(body: ReceiverPackage):
    try:
        url = package_service.normalize_receiver_url(body.url)
    except ValueError as exc:
        raise HTTPException(400, str(exc)) from exc
    if body.platform == 'windows-amd64':
        try:
            binary = package_service.windows_receiver().read_bytes()
        except (ValueError, OSError) as exc:
            raise HTTPException(503, str(exc)) from exc
        return Response(binary, media_type='application/octet-stream',
                        headers={'Content-Disposition': f'attachment; filename="{package_service.WINDOWS_FILENAME}"',
                                 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff'})
    try:
        archive = package_service.python_receiver(url)
    except OSError as exc:
        raise HTTPException(503, '无法读取电脑接收程序') from exc
    return Response(archive, media_type='application/zip',
                    headers={'Content-Disposition': 'attachment; filename="dataset-receiver.zip"',
                             'Cache-Control': 'no-store'})


# Only transfer endpoints are public. Task configuration/downloads still use
# normal console authentication in main.py; the receiver needs no login.
@router.post('/dataset-receiver/poll')
def poll(body: Heartbeat):
    manager.heartbeat(body.client, body.directory, body.error,
                      body.inventory.model_dump() if body.inventory is not None else None)
    return {'samples': manager.pending() if not body.error else []}


@router.post('/dataset-receiver/disconnect')
def disconnect(body: ReceiverIdentity):
    return {'disconnected': manager.disconnect(body.client)}


@router.get('/dataset-receiver/image')
def image(app: str, task_id: str, sample_id: str):
    data = manager.image(app, task_id, sample_id)
    return Response(data, media_type='image/jpeg', headers={'Cache-Control': 'no-store'})


@router.post('/dataset-receiver/ack')
def ack(body: Ack):
    saved = manager.acknowledge(**body.model_dump())
    return {'saved': saved}
