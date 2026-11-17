"""Authenticated HTTP adapter; model execution stays in the native engine."""
import json
import os
from pathlib import Path
from typing import List

from fastapi import APIRouter, File, Form, HTTPException, UploadFile
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field, StrictBool

from services.image_inference.engine import EngineCatalog, JobError, MAX_IMAGE_BYTES, MAX_UPLOAD_BYTES
from services.image_inference.jobs import ImageJobs
from services.image_inference.sources import scan_device_images
from services.image_inference.assets import ModelAssets
from services.image_inference.preset import resolve_canvas_preset

console_root = Path(os.environ.get('APPS_ROOT', '/opt/ai_apps')) / '_console'
catalog = EngineCatalog(Path(os.environ.get('IMAGE_INFERENCE_RUNTIME', str(console_root / 'inference_runtime'))))
image_jobs = ImageJobs(Path(os.environ.get('IMAGE_INFERENCE_ROOT', str(console_root / 'image_inference'))), catalog)
model_assets = ModelAssets(Path(os.environ.get('IMAGE_INFERENCE_MODELS', str(console_root / 'model_assets'))))
router = APIRouter(prefix='/image-inference', tags=['image-inference'])


class DevicePath(BaseModel):
    path: str = Field(min_length=1, max_length=4096)
    recursive: StrictBool = False


class DeviceJob(DevicePath):
    settings: dict


class CanvasPreset(BaseModel):
    app: str
    settings: dict


async def call(function, *args):
    try:
        return await run_in_threadpool(function, *args)
    except JobError as exc:
        raise HTTPException(exc.status, str(exc)) from exc
    except OSError as exc:
        raise HTTPException(503, f'图片任务存储不可用：{exc.strerror}') from exc


@router.get('/capabilities')
async def capabilities():
    return await call(catalog.capabilities)


@router.get('/jobs/current')
async def current_job():
    return await call(image_jobs.current)


@router.delete('/jobs/completed')
async def clear_completed_jobs():
    return await call(image_jobs.clear_completed)


@router.get('/assets')
async def assets():
    return await call(model_assets.list)


@router.get('/models/info')
async def model_info(path: str, model_type: str):
    return await call(catalog.inspect_model, path, model_type)


@router.post('/assets/upload')
async def upload_asset(file: UploadFile = File(...)):
    try:
        return await call(model_assets.save, file.filename, file.file)
    finally:
        await file.close()


@router.post('/canvas-preset')
async def canvas_preset(request: CanvasPreset):
    return await call(resolve_canvas_preset, console_root.parent, request.app, request.settings)


@router.post('/device-images')
async def device_images(source: DevicePath):
    selection = await call(scan_device_images, source.path, source.recursive)
    return selection.preview()


@router.post('/jobs/from-device', status_code=202)
async def create_device_job(request: DeviceJob):
    if len(json.dumps(request.settings, ensure_ascii=False)) > 65536:
        raise HTTPException(422, '推理参数过长')
    return await call(image_jobs.create_from_device, request.settings, request.path, request.recursive)


@router.post('/jobs', status_code=202)
async def create_job(settings: str = Form(...), files: List[UploadFile] = File(...)):
    job_id = None
    submitted = False
    try:
        if len(settings) > 65536:
            raise HTTPException(422, '推理参数过长')
        try:
            options = json.loads(settings)
        except ValueError as exc:
            raise HTTPException(422, '推理参数 JSON 无效') from exc
        job_id = await call(image_jobs.reserve, options, [f.filename for f in files])
        total = 0
        for index, upload in enumerate(files):
            path = await call(image_jobs.input_path, job_id, index)
            size = 0
            with path.open('xb') as target:
                while True:
                    chunk = await upload.read(1024 * 1024)
                    if not chunk:
                        break
                    size += len(chunk)
                    total += len(chunk)
                    if size > MAX_IMAGE_BYTES or total > MAX_UPLOAD_BYTES:
                        raise HTTPException(413, '单张图片限 20 MiB，本批图片总大小限 256 MiB')
                    await run_in_threadpool(target.write, chunk)
        result = await call(image_jobs.submit, job_id)
        submitted = True
        return result
    finally:
        if job_id and not submitted:
            await call(image_jobs.discard_upload, job_id)
        for upload in files:
            await upload.close()


@router.post('/jobs/{job_id}/cancel')
async def cancel(job_id: str):
    return await call(image_jobs.cancel, job_id)


@router.get('/jobs/{job_id}/items/{index}')
async def item(job_id: str, index: int):
    return await call(image_jobs.result, job_id, index)


@router.get('/jobs/{job_id}/download')
async def download(job_id: str):
    path = await call(image_jobs.archive, job_id)
    return FileResponse(path, media_type='application/zip', filename=f'image-inference-{job_id[:8]}.zip')


@router.get('/jobs/{job_id}/files/{path:path}')
async def artifact(job_id: str, path: str, download: bool = False):
    target = await call(image_jobs.artifact, job_id, path)
    return FileResponse(target, filename=target.name if download else None,
                        headers={'Cache-Control': 'private, no-store'})
