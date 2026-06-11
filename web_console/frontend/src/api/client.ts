import axios from 'axios'
import { useAuthStore } from '../store/authStore'

const api = axios.create({ baseURL: '/api' })

// ── Request: attach Bearer token ──────────────────────────────────────────
api.interceptors.request.use(config => {
  const token = useAuthStore.getState().token
  if (token) config.headers.Authorization = `Bearer ${token}`
  return config
})

// ── Response: 401 → clear auth and redirect to /login ────────────────────
api.interceptors.response.use(
  res => res,
  err => {
    if (err.response?.status === 401) {
      useAuthStore.getState().clearAuth()
      if (!window.location.pathname.startsWith('/login')) {
        window.location.href = '/login'
      }
    }
    return Promise.reject(err)
  }
)

// ── Auth ──────────────────────────────────────────────────────────────────
export const apiLogin = (username: string, password: string) =>
  api.post<{ token: string; username: string }>('/auth/login', { username, password }).then(r => r.data)

export const apiLogout = () =>
  api.post('/auth/logout').then(r => r.data)

// ── App management ────────────────────────────────────────────────────────
export interface AppInfo {
  name: string
  path: string
  has_binary: boolean
  has_config: boolean
  models: string[]
  labels: string[]
  videos: string[]
  config_files: string[]        // assets/ 下可选作启动配置的 .json（排除 roi_zones.json）
  active_config: string         // 上次/默认启动所用的配置文件名
  status: 'running' | 'stopped' | 'unknown'
  mode: string | null
  pid: number | null
  uptime_seconds: number | null
  config?: string | null        // 运行中时实际加载的配置文件名
}

export interface ConsoleInfo {
  version: string
  apps_root: string
  binary_name: string
  known_channel_logics: string[]
  known_global_logics: string[]
  known_model_types: string[]
}

export interface AppAssets {
  models: string[]
  labels: string[]
  videos: string[]
}

export const fetchApps = () => api.get<AppInfo[]>('/apps').then(r => r.data)

export const fetchStatus = (name: string) =>
  api.get<Pick<AppInfo, 'status' | 'mode' | 'pid' | 'uptime_seconds' | 'config'>>(`/apps/${name}/status`).then(r => r.data)

// config: 指定运行的配置文件名（assets/ 下，默认 config.json）。不传则用 config.json。
export const startApp = (name: string, mode: 'deploy' | 'debug', config?: string) =>
  api.post(`/apps/${name}/start`, { mode, config }).then(r => r.data)

export const stopApp = (name: string) =>
  api.post(`/apps/${name}/stop`).then(r => r.data)

export const fetchConfig = (name: string) =>
  api.get<Record<string, unknown> | null>(`/apps/${name}/config`).then(r => r.data)

export const saveConfig = (name: string, config: Record<string, unknown>) =>
  api.post(`/apps/${name}/config`, config).then(r => r.data)

// 保存到 assets/ 下指定文件（「另存为」/编辑非默认配置）。path 可为 'x.json' 或 'assets/x.json'。
export const saveConfigFile = (name: string, path: string, config: Record<string, unknown>) =>
  api.post<{ ok: boolean; path: string }>(
    `/apps/${name}/config-file`, config, { params: { path } },
  ).then(r => r.data)

// 删除 assets/ 下指定配置文件（roi_zones.json 受保护）。
export const deleteConfigFile = (name: string, path: string) =>
  api.delete<{ ok: boolean }>(`/apps/${name}/config-file`, { params: { path } }).then(r => r.data)

// 一个通道可有多个 ROI 区域: zones=全部区域(名字+多边形), polygon=首区域(向后兼容单 ROI)。
export type RoiEntry = {
  polygon?: number[][]
  zones?: { name: string; polygon: number[][] }[]
}

export const fetchROI = (name: string) =>
  api.get<Record<string, RoiEntry>>(`/apps/${name}/roi`).then(r => r.data)

export const saveROI = (name: string, roi: Record<string, RoiEntry>) =>
  api.post(`/apps/${name}/roi`, roi).then(r => r.data)

export const fetchAssets = (name: string) =>
  api.get<AppAssets>(`/apps/${name}/assets`).then(r => r.data)

// 导入一个视频/模型/标签文件到 assets/。重名且 overwrite=false → 后端另存为 _copy。
export const uploadAsset = (name: string, file: File, overwrite: boolean) => {
  const fd = new FormData()
  fd.append('file', file)
  fd.append('overwrite', overwrite ? 'true' : 'false')
  return api.post<{ ok: boolean; path: string; name: string; category: string; renamed: boolean }>(
    `/apps/${name}/assets/upload`, fd,
  ).then(r => r.data)
}

export const fetchConsoleInfo = () =>
  api.get<ConsoleInfo>('/console/info').then(r => r.data)

export const fetchLogTail = (name: string, lines = 300) =>
  api.get<{ lines: string[] }>(`/apps/${name}/log?lines=${lines}`).then(r => r.data)

export const fetchConfigFiles = (name: string) =>
  api.get<string[]>(`/apps/${name}/config-files`).then(r => r.data)

export const loadConfigFile = (name: string, path: string) =>
  api.get<Record<string, unknown>>(`/apps/${name}/config-file`, { params: { path } }).then(r => r.data)

export interface SnapshotResult {
  image: string   // data:image/jpeg;base64,...
  width: number
  height: number
}

export const captureSnapshot = (
  appName: string,
  params: { src_type: string; url?: string; device?: string; usb_width?: number; usb_height?: number }
) => api.get<SnapshotResult>(`/apps/${appName}/snapshot`, { params }).then(r => r.data)

// ── 逻辑参数清单(由 app 根目录 logics.json 提供, 后端 /apps/{name}/logics 透传) ──
export interface LogicParam {
  key: string                                            // = config.json 键 = ChannelConfig 字段名
  type: 'int' | 'float' | 'string' | 'bool' | 'enum' | 'text'
  label?: string
  default?: unknown
  min?: number
  max?: number
  options?: string[]
  placeholder?: string
  help?: string
}
export interface LogicDef {
  name: string
  label?: string
  report?: 'dify' | 'server'                             // 需要连接「上报配置」节点
  params?: LogicParam[]
}

export interface AppLogics {
  // 兼容两种格式: 富对象(来自 logics.json) 或 仅名字字符串(回退)
  channel_logics: (string | LogicDef)[]
  global_logics:  (string | LogicDef)[]
  model_types:    string[]
}

// 归一化: 名字字符串 → LogicDef
export const asLogicDef = (l: string | LogicDef): LogicDef =>
  typeof l === 'string' ? { name: l } : l

export const fetchAppLogics = (name: string) =>
  api.get<AppLogics>(`/apps/${name}/logics`).then(r => r.data)

// ── 微服务配置 (上报服务 config.yaml 默认值 + OTA 升级服务 ota_config.json) ──
export interface UploadServiceConfig {
  dify:   { api_url: string; api_key: string; timeout: number }
  server: { url: string; timeout: number }
  redis:  { host: string; port: number; db: number; dify_queue: string; server_queue: string }
}
export interface OtaConfig {
  platform_ws_host: string
  target_config: string
}

export const fetchUploadConfig = (name: string) =>
  api.get<UploadServiceConfig>(`/apps/${name}/upload-config`).then(r => r.data)
export const saveUploadConfig = (name: string, cfg: UploadServiceConfig) =>
  api.post(`/apps/${name}/upload-config`, cfg).then(r => r.data)
export const fetchOtaConfig = (name: string) =>
  api.get<OtaConfig>(`/apps/${name}/ota-config`).then(r => r.data)
export const saveOtaConfig = (name: string, cfg: OtaConfig) =>
  api.post(`/apps/${name}/ota-config`, cfg).then(r => r.data)

// ── Live MJPEG stream URL (用于 <img src>) ─────────────────────────────────
// <img> 无法携带 Authorization 头，token 走查询参数 (后端 auth_middleware 已放行)。
export const streamUrl = (name: string, fps = 10): string => {
  const token = useAuthStore.getState().token ?? ''
  return `/api/apps/${encodeURIComponent(name)}/stream?fps=${fps}&token=${encodeURIComponent(token)}`
}

// ── 板端后台服务 (systemd 单元: OTA 升级 / 告警上报) ────────────────────────
export interface ServiceInfo {
  key: string
  label: string
  unit: string
  installed: boolean
  active_state: string          // active / inactive / failed / activating / unknown
  sub_state: string
  enabled: boolean
  uptime_seconds: number | null
  n_restarts: number | null
  bound_app: string | null      // 单元当前绑定到哪个 App 的 services/
  working_dir: string | null    // 单元的 WorkingDirectory（用于判断是否失效）
  path_ok: boolean              // WorkingDirectory 是否真实存在；false=失效单元，需重装修正
}

export const fetchServices = () => api.get<ServiceInfo[]>('/services').then(r => r.data)

export const installService = (key: string, app: string) =>
  api.post(`/services/${key}/install`, { app }).then(r => r.data)

export const controlService = (key: string, action: 'start' | 'stop' | 'restart') =>
  api.post(`/services/${key}/${action}`).then(r => r.data)

export const fetchServiceLogs = (key: string, lines = 200) =>
  api.get<{ lines: string[] }>(`/services/${key}/logs`, { params: { lines } }).then(r => r.data)

// ── 程序包上传 / 删除 ──────────────────────────────────────────────────────
export const uploadApp = (
  file: File,
  name: string | undefined,
  onProgress?: (pct: number) => void,
) => {
  const fd = new FormData()
  fd.append('file', file)
  if (name) fd.append('name', name)
  return api.post<{ ok: boolean; name: string; has_binary: boolean; has_config: boolean }>(
    '/apps/upload', fd,
    { onUploadProgress: e => { if (onProgress && e.total) onProgress(Math.round((e.loaded / e.total) * 100)) } },
  ).then(r => r.data)
}

export const deleteApp = (name: string) =>
  api.delete(`/apps/${name}`).then(r => r.data)

