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
  max_images: number
  max_image_bytes: number
  max_upload_bytes: number
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
export const createImageJob = (settings: ImageSettings, files: File[], onProgress: (n: number) => void) => {
  const form = new FormData()
  form.append('settings', JSON.stringify(settings))
  files.forEach(file => form.append('files', file))
  return api.post<ImageJob>(`${base}/jobs`, form, {
    onUploadProgress: e => { if (e.total) onProgress(Math.round(e.loaded / e.total * 100)) },
  }).then(r => r.data)
}
export const inspectDeviceImages = (path: string, recursive: boolean) =>
  api.post<DeviceImages>(`${base}/device-images`, { path, recursive }).then(r => r.data)
export const createDeviceImageJob = (settings: ImageSettings, path: string, recursive: boolean) =>
  api.post<ImageJob>(`${base}/jobs/from-device`, { settings, path, recursive }).then(r => r.data)
export function imageArtifactUrl(id: string, index: number, file: string, download = false): string {
  const query = new URLSearchParams({ token: useAuthStore.getState().token ?? '' })
  if (download) query.set('download', 'true')
  return `/api${task(id)}/files/${String(index).padStart(6, '0')}/${encodeURIComponent(file)}?${query}`
}
export function imageArchiveUrl(id: string): string {
  return `/api${task(id)}/download?${new URLSearchParams({ token: useAuthStore.getState().token ?? '' })}`
}

export interface ImageAssets { models: string[]; labels: string[] }
export interface UploadedImageAsset { path: string; name: string; renamed: boolean; size: number }
export const fetchImageAssets = () => api.get<ImageAssets>(`${base}/assets`).then(r => r.data)
export const uploadImageAsset = (file: File, onProgress: (value: number) => void) => {
  const form = new FormData()
  form.append('file', file)
  return api.post<UploadedImageAsset>(`${base}/assets/upload`, form, {
    onUploadProgress: event => { if (event.total) onProgress(Math.round(event.loaded / event.total * 100)) },
  }).then(r => r.data)
}
export const resolveCanvasImageSettings = (app: string, settings: ImageSettings) =>
  api.post<ImageSettings>(`${base}/canvas-preset`, { app, settings }).then(r => r.data)
