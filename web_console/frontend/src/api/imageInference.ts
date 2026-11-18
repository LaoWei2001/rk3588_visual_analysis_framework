import { isAxiosError } from 'axios'
import { api } from './http'
import { useAuthStore } from '../store/authStore'
import type { ModelInfo } from './apps'

export interface ImageModel {
  id: string
  model_type: string
  model_path: string
  label_path: string
  obj_thresh: number
  nms_thresh: number
  npu_core: number
  detect_classes: string[]
}
export interface ImageSettings {
  models: ImageModel[]
  inference_roi?: { mode: string; resize_mode: string; polygon: number[][] }
}
export interface ImageCapabilities {
  models: { type: string; result_kind: string; requires_labels: boolean }[]
  max_models: number
  free_bytes: number
  min_free_bytes: number
  upload_available_bytes: number
  busy: boolean
}
export interface ImageItem {
  index: number
  name: string
  status: string
  result?: string
  elapsed_ms?: number
  error?: string
  source_path?: string
}
export interface ImageSource {
  kind: 'upload' | 'device'
  path?: string
  recursive?: boolean
  is_directory?: boolean
}
export interface DeviceImages {
  path: string
  is_directory: boolean
  recursive: boolean
  total: number
  total_bytes: number
  images: { name: string; path: string; size: number }[]
}
export interface ImageJob {
  id: string
  status: string
  created_at: number
  finished_at?: number
  settings: ImageSettings
  source?: ImageSource
  items: ImageItem[]
  error: string
  total: number
  completed: number
}
export interface ImageObject {
  model_id: string
  model_type: string
  label: string
  class_id: number
  score: number
  track_id: number
  box: { x: number; y: number; width: number; height: number }
  obb_corners?: number[][]
  keypoints?: number[][]
  keypoint_scores?: number[]
  pose_schema?: string
  depth_kind?: string
}
export interface ImageResult {
  source_width: number
  source_height: number
  coordinate_width: number
  coordinate_height: number
  elapsed_ms: number
  original: string
  annotated: string
  models: {
    id: string; type: string; succeeded: boolean; error: string
    input_width: number; input_height: number
    preprocess_ms: number; infer_ms: number; postprocess_ms: number
    annotated?: string; mask?: string; depth?: string; depth_kind?: string
  }[]
  results: ImageObject[]
}

const base = '/image-inference'
const task = (id: string) => `${base}/jobs/${encodeURIComponent(id)}`
export const imageJobFinished = (status: string) => ['completed', 'completed_with_errors', 'failed', 'cancelled'].includes(status)
export const fetchImageCapabilities = () => api.get<ImageCapabilities>(`${base}/capabilities`).then(r => r.data)
export const inspectImageModel = (path: string, modelType: string, signal?: AbortSignal) =>
  api.get<ModelInfo>(`${base}/models/info`, { params: { path, model_type: modelType }, signal }).then(r => r.data)
export const fetchCurrentImageJob = () => api.get<ImageJob | null>(`${base}/jobs/current`).then(r => r.data)
export const clearCompletedImageJobs = () =>
  api.delete<{ cleared: number; current: ImageJob | null }>(`${base}/jobs/completed`).then(r => r.data)
export const fetchImageResult = (id: string, index: number) =>
  api.get<ImageResult>(`${task(id)}/items/${index}`).then(r => r.data)
export const cancelImageJob = (id: string) => api.post<ImageJob>(`${task(id)}/cancel`).then(r => r.data)
export async function createImageJob(settings: ImageSettings, files: File[], onProgress: (n: number) => void) {
  const { data: batch } = await api.post<{ id: string }>(`${base}/jobs`, {
    settings, files: files.map(file => ({ name: file.name, size: file.size })),
  })
  let uploaded = 0
  const total = files.reduce((sum, file) => sum + file.size, 0)
  try {
    for (const [index, file] of files.entries()) {
      await api.put(`${task(batch.id)}/inputs/${index}`, file, {
        headers: { 'Content-Type': 'application/octet-stream' },
        onUploadProgress: event => onProgress(Math.min(100, Math.round((uploaded + event.loaded) / total * 100))),
      })
      uploaded += file.size
      onProgress(Math.round(uploaded / total * 100))
    }
    return (await api.post<ImageJob>(`${task(batch.id)}/submit`)).data
  } catch (error) {
    // Includes files already uploaded if a later file runs out of disk space.
    try { await api.delete(`${task(batch.id)}/upload`) } catch { /* Preserve the original error. */ }
    throw error
  }
}
export const inspectDeviceImages = (path: string, recursive: boolean) =>
  api.post<DeviceImages>(`${base}/device-images`, { path, recursive }).then(r => r.data)
export const createDeviceImageJob = (settings: ImageSettings, path: string, recursive: boolean) =>
  api.post<ImageJob>(`${base}/jobs/from-device`, { settings, path, recursive }).then(r => r.data)
export function imageArtifactUrl(id: string, index: number, file: string): string {
  const query = new URLSearchParams({ token: useAuthStore.getState().token ?? '' })
  return `/api${task(id)}/files/${String(index).padStart(6, '0')}/${encodeURIComponent(file)}?${query}`
}
export const canExportImageLabels = (job: ImageJob) =>
  job.settings.models.length === 1 && ['yolov5', 'yolov8_det'].includes(job.settings.models[0].model_type)

export async function downloadImageLabels(id: string, indices: number[]): Promise<Blob> {
  try {
    const response = await api.post<Blob>(`${task(id)}/labels`, { indices }, { responseType: 'blob' })
    return response.data
  } catch (error) {
    // Axios also returns HTTP errors as blobs for a binary response.
    if (isAxiosError(error) && error.response?.data instanceof Blob) {
      try { error.response.data = JSON.parse(await error.response.data.text()) } catch { /* Keep the HTTP error. */ }
    }
    throw error
  }
}
