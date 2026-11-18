"""Authenticated HTTP adapter; model execution stays in the native engine."""
import json
import os
from pathlib import Path
from typing import List

from fastapi import APIRouter, HTTPException, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import FileResponse, Response
from pydantic import BaseModel, Field, StrictBool, StrictInt

from services.image_inference.engine import EngineCatalog, JobError
from services.image_inference.jobs import ImageJobs
from services.image_inference.sources import scan_device_images
from services.image_inference.storage import require_space

console_root = Path(os.environ.get('APPS_ROOT', '/opt/ai_apps')) / '_console'
catalog = EngineCatalog(Path(os.environ.get('IMAGE_INFERENCE_RUNTIME', str(console_root / 'inference_runtime'))))
image_jobs = ImageJobs(Path(os.environ.get('IMAGE_INFERENCE_ROOT', str(console_root / 'image_inference'))), catalog)
router = APIRouter(prefix='/image-inference', tags=['image-inference'])


class DevicePath(BaseModel):
    path: str = Field(min_length=1, max_length=4096)
    recursive: StrictBool = False


class DeviceJob(DevicePath):
    settings: dict


class LabelSelection(BaseModel):
    indices: List[StrictInt] = Field(min_length=1)


class UploadImage(BaseModel):
    name: str = Field(min_length=1, max_length=240)
    size: StrictInt = Field(gt=0)


class UploadJob(BaseModel):
    settings: dict
    files: List[UploadImage] = Field(min_length=1)


async def call(function, *args):
    try:
        return await run_in_threadpool(function, *args)
    except JobError as exc:
        raise HTTPException(exc.status, str(exc)) from exc
    except OSError as exc:
        raise HTTPException(503, f'图片任务存储不可用：{exc.strerror}') from exc


@router.get('/capabilities')
async def capabilities():
    return {**await call(catalog.capabilities), **await call(image_jobs.storage)}


@router.get('/jobs/current')
async def current_job():
    return await call(image_jobs.current)


@router.delete('/jobs/completed')
async def clear_completed_jobs():
    return await call(image_jobs.clear_completed)


@router.get('/models/info')
async def model_info(path: str, model_type: str):
    return await call(catalog.inspect_model, path, model_type)


@router.post('/device-images')
async def device_images(source: DevicePath):
    selection = await call(scan_device_images, source.path, source.recursive)
    await call(require_space, image_jobs.root, sum(image.size for image in selection.images))
    return selection.preview()


@router.post('/jobs/from-device', status_code=202)
async def create_device_job(request: DeviceJob):
    if len(json.dumps(request.settings, ensure_ascii=False)) > 65536:
        raise HTTPException(422, '推理参数过长')
    return await call(image_jobs.create_from_device, request.settings, request.path, request.recursive)


@router.post('/jobs', status_code=202)
async def create_job(request: UploadJob):
    if len(json.dumps(request.settings, ensure_ascii=False)) > 65536:
        raise HTTPException(422, '推理参数过长')
    job_id = await call(image_jobs.reserve, request.settings, [file.name for file in request.files],
                        None, [file.size for file in request.files])
    return {'id': job_id}


@router.put('/jobs/{job_id}/inputs/{index}')
async def upload_image(job_id: str, index: int, request: Request):
    # Stream directly to the job: no multipart file limit or temporary second copy.
    path = await call(image_jobs.input_path, job_id, index)
    expected = await call(image_jobs.upload_size, job_id, index)
    try:
        with path.open('xb') as output:
            received = 0
            async for chunk in request.stream():
                received += len(chunk)
                if received > expected:
                    raise HTTPException(422, '图片大小与上传清单不一致')
                await call(image_jobs.write_input, job_id, index, output, chunk)
            if received != expected:
                raise HTTPException(422, '图片上传不完整')
        return {'received': received}
    except FileExistsError as exc:
        raise HTTPException(409, '该图片已上传或正在上传') from exc
    except BaseException as exc:
        try:
            await call(image_jobs.discard_upload, job_id)
        except HTTPException:
            pass  # Preserve the upload error if another request already cleared it.
        if isinstance(exc, OSError):
            raise HTTPException(503, f'图片任务存储不可用：{exc.strerror}') from exc
        raise


@router.post('/jobs/{job_id}/submit', status_code=202)
async def submit_job(job_id: str):
    return await call(image_jobs.submit, job_id)


@router.delete('/jobs/{job_id}/upload')
async def discard_upload(job_id: str):
    await call(image_jobs.discard_upload, job_id)
    return {'ok': True}


@router.post('/jobs/{job_id}/cancel')
async def cancel(job_id: str):
    return await call(image_jobs.cancel, job_id)


@router.get('/jobs/{job_id}/items/{index}')
async def item(job_id: str, index: int):
    return await call(image_jobs.result, job_id, index)


@router.post('/jobs/{job_id}/labels')
async def download_labels(job_id: str, selection: LabelSelection):
    content = await call(image_jobs.labels_archive, job_id, selection.indices)
    return Response(content, media_type='application/zip', headers={
        'Content-Disposition': f'attachment; filename="yolo-labels-{job_id[:8]}.zip"',
        'Cache-Control': 'private, no-store',
    })


@router.get('/jobs/{job_id}/files/{path:path}')
async def artifact(job_id: str, path: str):
    target = await call(image_jobs.artifact, job_id, path)
    return FileResponse(target, headers={'Cache-Control': 'private, no-store'})
