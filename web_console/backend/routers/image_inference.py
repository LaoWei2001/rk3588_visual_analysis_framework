"""Authenticated HTTP adapter; model execution stays in the native engine."""
import json
import errno
import os
from pathlib import Path
from typing import List

from fastapi import APIRouter, File, HTTPException, Query, Request, UploadFile
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import FileResponse, Response
from pydantic import BaseModel, Field, StrictBool, StrictInt

from services.asset_upload import save_asset
from services.image_inference.engine import EngineCatalog, JobError
from services.image_inference.jobs import ImageJobs
from services.image_inference.sources import list_device_folders, scan_device_images
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


async def call(function, *args, **kwargs):
    try:
        return await run_in_threadpool(function, *args, **kwargs)
    except JobError as exc:
        raise HTTPException(exc.status, str(exc)) from exc
    except OSError as exc:
        if exc.errno in (errno.ENOSPC, errno.EDQUOT):
            raise HTTPException(507, '存储空间不足，请清理空间后继续') from exc
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


@router.get('/assets')
def list_inference_assets():
    directory = console_root / 'model_assets'
    result = {'models': [], 'labels': []}
    if directory.is_dir():
        for path in sorted(directory.iterdir()):
            suffix = path.suffix.lower()
            if suffix in ('.rknn', '.txt') and not path.is_symlink() and path.is_file():
                result['models' if suffix == '.rknn' else 'labels'].append(str(path))
    return result


@router.post('/assets/upload')
async def upload_inference_asset(file: UploadFile = File(...)):
    name = Path(file.filename or '').name
    if Path(name).suffix.lower() not in ('.rknn', '.txt'):
        raise HTTPException(422, '请选择 .rknn 模型或 .txt 标签文件')
    if not file.size or file.size > 1024 ** 3:
        raise HTTPException(422, '文件不能为空且不能超过 1 GiB')
    directory = console_root / 'model_assets'
    await call(require_space, directory, file.size)
    await call(directory.mkdir, mode=0o700, parents=True, exist_ok=True)
    target = await call(save_asset, directory, name, file.file)
    return {'path': str(target), 'renamed': target.name != name}


@router.get('/models/info')
async def model_info(path: str, model_type: str):
    return await call(catalog.inspect_model, path, model_type)


@router.get('/device-folders')
async def device_folders(path: str = Query('/', min_length=1, max_length=4096)):
    return await call(list_device_folders, path)


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
        await call(image_jobs.upload_complete, job_id, index)
        return {'received': received}
    except FileExistsError as exc:
        raise HTTPException(409, '该图片已上传或正在上传') from exc
    except BaseException as exc:
        if (isinstance(exc, HTTPException) and exc.status_code == 507 or
                isinstance(exc, OSError) and exc.errno in (errno.ENOSPC, errno.EDQUOT)):
            await call(image_jobs.pause_upload, job_id, index, '存储空间不足，请清理空间后继续上传')
            raise HTTPException(507, '存储空间不足，请清理空间后继续上传') from exc
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


@router.post('/jobs/{job_id}/resume', status_code=202)
async def resume(job_id: str):
    return await call(image_jobs.resume, job_id)


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
