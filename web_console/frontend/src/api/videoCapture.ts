import axios from 'axios'
import { useAuthStore } from '../store/authStore'


const captureApi = axios.create({ baseURL: '/api/video-capture' })

captureApi.interceptors.request.use(config => {
  const token = useAuthStore.getState().token
  if (token) config.headers.Authorization = `Bearer ${token}`
  return config
})

captureApi.interceptors.response.use(
  response => response,
  error => {
    if (error.response?.status === 401) {
      useAuthStore.getState().clearAuth()
      if (!window.location.pathname.startsWith('/login')) window.location.href = '/login'
    }
    return Promise.reject(error)
  },
)

export const videoCaptureErrorMessage = (error: unknown): string => {
  if (axios.isAxiosError(error)) {
    const detail = error.response?.data?.detail ?? error.response?.data?.message
    return typeof detail === 'string' ? detail : error.message
  }
  return error instanceof Error ? error.message : String(error)
}

export type CaptureSourceType = 'rtsp' | 'usb'

export interface CaptureSourceInput {
  source_type: CaptureSourceType
  rtsp_url: string
  usb_device: string
  usb_width: number
  usb_height: number
}

export interface UsbCaptureResolution {
  width: number
  height: number
  max_fps: number
}

export interface UsbCaptureDevice {
  device: string
  label: string
  readable: boolean
  resolutions: UsbCaptureResolution[]
}

export interface CaptureStorageInfo {
  path: string
  mount_point: string
  filesystem: string
  writable: boolean
  total_bytes: number
  used_bytes: number
  free_bytes: number
  available_bytes: number
  reserve_bytes: number
  safe_available_bytes: number
  filesystem_max_file_bytes: number | null
  max_recording_file_bytes: number
  allowed_roots: string[]
}

export interface CaptureProbe {
  codec: string
  input_codec?: string
  width: number
  height: number
  fps: number
  bitrate: number
}

export interface CaptureStatus {
  state: 'idle' | 'previewing' | 'recording' | 'stopping' | 'completed' | 'error'
  recording: boolean
  previewing: boolean
  source: { source_type: CaptureSourceType; label: string } | null
  probe: CaptureProbe | null
  started_unix_ms: number | null
  elapsed_seconds: number
  output_path: string | null
  file_size_bytes: number
  max_file_size_bytes: number
  stop_reason: string | null
  error: string | null
  process_alive: boolean
  storage: CaptureStorageInfo | null
}

export const fetchCaptureDevices = () =>
  captureApi.get<{ devices: UsbCaptureDevice[] }>('/devices').then(response => response.data.devices)

export const fetchCaptureStorage = (path: string) =>
  captureApi.get<CaptureStorageInfo>('/storage', { params: { path } }).then(response => response.data)

export const fetchCaptureStatus = () =>
  captureApi.get<CaptureStatus>('/status').then(response => response.data)

export const startCapturePreview = (source: CaptureSourceInput) =>
  captureApi.post<CaptureStatus>('/preview/start', source).then(response => response.data)

export const stopCapturePreview = () =>
  captureApi.post<CaptureStatus>('/preview/stop').then(response => response.data)

export const startCaptureRecording = (
  source: CaptureSourceInput,
  savePath: string,
  maxFileSizeMb: number,
) => captureApi.post<CaptureStatus>('/recordings/start', {
  ...source,
  save_path: savePath,
  max_file_size_mb: maxFileSizeMb,
}).then(response => response.data)

export const stopCaptureRecording = () =>
  captureApi.post<CaptureStatus>('/recordings/stop').then(response => response.data)

export const capturePreviewStreamUrl = (nonce: number): string => {
  const token = useAuthStore.getState().token ?? ''
  return `/api/video-capture/preview/stream?t=${nonce}&token=${encodeURIComponent(token)}`
}
