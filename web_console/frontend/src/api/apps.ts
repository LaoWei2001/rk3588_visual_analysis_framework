import { api } from './http'

// ── App management ────────────────────────────────────────────────────────
export interface AppInfo {
  name: string
  path: string
  has_binary: boolean
  has_config: boolean
  models: string[]
  labels: string[]
  videos: string[]
  config_files: string[]        // assets/ 下可选作启动配置的 .json
  active_config: string         // 上次/默认启动所用的配置文件名
  unreported: number            // 本地发件箱里待上报的业务记录数
  autostart: boolean            // 用户是否勾选“开机自启”
  desired_running: boolean      // 用户最后一次操作是否要求保持运行
  status: 'running' | 'stopped' | 'unknown'
  pid: number | null
  uptime_seconds: number | null
  recovery_state?: 'running' | 'restarting' | 'stopping'
  shutdown_stage?: string
  restart_count?: number
  config?: string | null        // 运行中时实际加载的配置文件名
}

export interface ConsoleInfo {
  version: string
  apps_root: string
  binary_name: string
  known_model_types: string[]
}

export interface AppAssets {
  models: string[]
  labels: string[]
  videos: string[]
}

export interface ModelInfo {
  ok: boolean
  error?: string
  path: string
  file_version: string
  width?: number
  height?: number
  format?: string
}

export interface ModelRuntime {
  status: string
  config: string | null
  error?: string
  business_width?: number
  business_height?: number
  channels: Array<{
    channel_id: number
    state: 'loading' | 'applied' | 'failed' | 'disabled' | 'stopped'
    error: string
    active_models: Array<{
      id: string; model_type: string; model_path: string; file_version: string
      width: number; height: number
    }>
  }>
}

export const fetchModelInfo = (name: string, path: string, modelType: string) =>
  api.get<ModelInfo>(`/apps/${encodeURIComponent(name)}/models/info`, {
    params: { path, model_type: modelType },
  }).then(r => r.data)

export const fetchModelRuntime = (name: string) =>
  api.get<ModelRuntime>(`/apps/${encodeURIComponent(name)}/models/runtime`, {
    params: { _status_ts: Date.now() },
  }).then(r => r.data)

export const fetchApps = () => api.get<AppInfo[]>('/apps', {
  params: { _status_ts: Date.now() },
}).then(r => r.data)

// config: 指定运行的配置文件名（assets/ 下，默认 config.json）。不传则用 config.json。
export const startApp = (name: string, config?: string) =>
  api.post(`/apps/${name}/start`, { config }).then(r => r.data)

export interface StopResult {
  ok: boolean
  stopped: boolean
  graceful: boolean | null
  exit_code: number | null
  message: string
  warnings: string[]
}

export const stopApp = (name: string) =>
  api.post<StopResult>(`/apps/${name}/stop`).then(r => r.data)

export const setAppAutostart = (name: string, enabled: boolean) =>
  api.post(`/apps/${name}/autostart`, { enabled }).then(r => r.data)

export const fetchConfig = (name: string) =>
  api.get<Record<string, unknown> | null>(`/apps/${name}/config`).then(r => r.data)

export const saveConfig = (name: string, config: Record<string, unknown>) =>
  api.post(`/apps/${name}/config`, config).then(r => r.data)

// 保存到 assets/ 下指定文件（「另存为」/编辑非默认配置）。path 可为 'x.json' 或 'assets/x.json'。
export const saveConfigFile = (name: string, path: string, config: Record<string, unknown>) =>
  api.post<{ ok: boolean; path: string }>(
    `/apps/${name}/config-file`, config, { params: { path } },
  ).then(r => r.data)

// 删除 assets/ 下指定配置文件。
export const deleteConfigFile = (name: string, path: string) =>
  api.delete<{ ok: boolean }>(`/apps/${name}/config-file`, { params: { path } }).then(r => r.data)

export const fetchAssets = (name: string) =>
  api.get<AppAssets>(`/apps/${name}/assets`).then(r => r.data)

// 导入一个视频/模型/标签文件到 assets/。重名且 overwrite=false → 后端另存为 _copy。
// onProgress: 上传进度回调（0–100），大文件（模型/视频）用来显示进度条。
export const uploadAsset = (
  name: string,
  file: File,
  overwrite: boolean,
  onProgress?: (pct: number) => void,
) => {
  const fd = new FormData()
  fd.append('file', file)
  fd.append('overwrite', overwrite ? 'true' : 'false')
  return api.post<{ ok: boolean; path: string; name: string; category: string; renamed: boolean }>(
    `/apps/${name}/assets/upload`, fd,
    { onUploadProgress: e => { if (onProgress && e.total) onProgress(Math.round((e.loaded / e.total) * 100)) } },
  ).then(r => r.data)
}

export const deleteAsset = (name: string, path: string) =>
  api.delete<{ ok: boolean; path: string; category: string }>(`/apps/${name}/assets`, { params: { path } }).then(r => r.data)

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

// ── 程序包上传 / 删除 ──────────────────────────────────────────────────────
export const uploadApp = (
  file: File,
  name: string | undefined,
  onProgress?: (pct: number) => void,
) => {
  const fd = new FormData()
  fd.append('file', file)
  if (name) fd.append('name', name)
  return api.post<{
    ok: boolean
    name: string
    has_binary: boolean
    has_config: boolean
    stopped_apps: string[]
  }>(
    '/apps/upload', fd,
    { onUploadProgress: e => { if (onProgress && e.total) onProgress(Math.round((e.loaded / e.total) * 100)) } },
  ).then(r => r.data)
}

export const deleteApp = (name: string, deleteData = false) =>
  api.delete(`/apps/${name}`, { params: { delete_data: deleteData } }).then(r => r.data)
