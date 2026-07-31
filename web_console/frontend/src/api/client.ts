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
  config_files: string[]        // assets/ 下可选作启动配置的 .json
  active_config: string         // 上次/默认启动所用的配置文件名
  unreported: number            // 本地发件箱里待上报的业务记录数
  autostart: boolean            // 用户是否勾选“开机自启”
  desired_running: boolean      // 用户最后一次操作是否要求保持运行
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
  known_model_types: string[]
}

export interface AppAssets {
  models: string[]
  labels: string[]
  videos: string[]
}

export const fetchApps = () => api.get<AppInfo[]>('/apps', {
  params: { _status_ts: Date.now() },
}).then(r => r.data)

export const fetchStatus = (name: string) =>
  api.get<Pick<AppInfo, 'status' | 'mode' | 'pid' | 'uptime_seconds' | 'config'>>(
    `/apps/${name}/status`, { params: { _status_ts: Date.now() } },
  ).then(r => r.data)

// config: 指定运行的配置文件名（assets/ 下，默认 config.json）。不传则用 config.json。
export const startApp = (name: string, mode: 'deploy' | 'debug', config?: string) =>
  api.post(`/apps/${name}/start`, { mode, config }).then(r => r.data)

export const stopApp = (name: string) =>
  api.post(`/apps/${name}/stop`).then(r => r.data)

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

// ── 逻辑参数清单(由 app 根目录 logics.json 提供, 后端 /apps/{name}/logics 透传) ──
export interface LogicParam {
  key: string
  type: 'int' | 'float' | 'string' | 'bool' | 'enum' | 'text' | 'json'
  json_type?: 'array' | 'object'                         // type=json 时约束容器类型
  label?: string
  default?: unknown
  min?: number
  max?: number
  step?: number
  unit?: string
  hot_reload?: 'preserve_state' | 'reset_state' | 'restart_required'
  options?: string[]
  placeholder?: string
  help?: string
}
export interface ReportField {
  key: string
  type: 'string' | 'number' | 'boolean' | 'json'
  label?: string
  help?: string
}
export interface EventTypeDef {
  id: string
  label?: string
  help?: string
}
export interface BusinessField {
  path: string
  type: 'string' | 'number' | 'boolean' | 'json'
  label?: string
  help?: string
  required?: boolean
  default_selected?: boolean
}
export interface LogicDef {
  name: string
  label?: string
  params?: LogicParam[]
  parameters?: Record<string, unknown>                    // 模块 JSON Schema（C++/Web 同源）
  event_types?: EventTypeDef[]                            // 本逻辑可产生的 EventRequest.event_type
  report_fields?: ReportField[]                            // C++ EventRequest.fields 只读字段清单
  business_fields?: BusinessField[]                        // 完整业务 JSON 字段目录
}

export interface AppLogics {
  // 兼容两种格式: 富对象(来自 logics.json) 或 仅名字字符串(回退)
  channel_logics: (string | LogicDef)[]
  global_logics:  (string | LogicDef)[]
  model_types:    string[]
  source: 'catalog' | 'binary' | 'unavailable'
  error?: string
}

// 归一化: 名字字符串 → LogicDef
export const asLogicDef = (l: string | LogicDef): LogicDef =>
  typeof l === 'string' ? { name: l } : l

export const fetchAppLogics = (name: string) =>
  api.get<AppLogics>(`/apps/${name}/logics`).then(r => r.data)

export interface LogicActionDef {
  id: string
  label?: string
  style?: 'default' | 'primary' | 'danger'
  confirm?: string
  help?: string
  payload?: Record<string, unknown>
}

export interface ChannelControlInfo {
  channel_id: number
  enabled: boolean
  logic: string
  logic_label: string
  actions: LogicActionDef[]
}

export interface ChannelControlsResponse {
  socket_ready: boolean
  channels: ChannelControlInfo[]
}

export const fetchChannelControls = (name: string) =>
  api.get<ChannelControlsResponse>(`/apps/${name}/channel-actions`).then(r => r.data)

export const sendChannelAction = (
  name: string,
  channelId: number,
  action: string,
  payload: Record<string, unknown> = {},
) => api.post(`/apps/${name}/channels/${channelId}/actions/${encodeURIComponent(action)}`, { payload }).then(r => r.data)

// ── 微服务配置 (上报服务 config.yaml 默认值 + OTA 升级服务 ota_config.json) ──
export interface UploadProfile {
  adapter: string
  [key: string]: unknown
}
export interface UploadServiceConfig {
  profiles: Record<string, UploadProfile>
}
export interface AdapterProfileField {
  key: string
  label: string
  type: 'string' | 'secret' | 'number' | 'json' | 'select'
  required?: boolean
  default?: unknown
  options?: string[]
}
export interface DeliveryAdapterDef {
  id: string
  label: string
  supported_media: Array<'annotated_image' | 'raw_image' | 'video'>
  profile_fields: AdapterProfileField[]
  transforms: string[]
}
export interface ReportContract {
  id: string
  label: string
  description?: string
  adapter: string
  media: Array<'annotated_image' | 'raw_image' | 'video'>
  mapping: Array<{
    source: string
    target: string
    value?: unknown
    type?: string
    transform?: string
    file_mode?: 'single' | 'list'
    required?: boolean
  }>
  request?: Record<string, unknown>
  success?: Record<string, unknown>
  source_file?: string
}
export interface OtaConfig {
  platform_ws_host: string
  target_config: string
}

export const fetchUploadConfig = (name: string) =>
  api.get<UploadServiceConfig>(`/apps/${name}/upload-config`).then(r => r.data)
export const saveUploadConfig = (name: string, cfg: UploadServiceConfig) =>
  api.post(`/apps/${name}/upload-config`, cfg).then(r => r.data)
export const fetchDeliveryAdapters = (name: string) =>
  api.get<{ adapters: DeliveryAdapterDef[] }>(`/apps/${name}/delivery-adapters`).then(r => r.data.adapters)
export const fetchReportContracts = (name: string) =>
  api.get<{ contracts: ReportContract[] }>(`/apps/${name}/report-contracts`).then(r => r.data.contracts)
export const saveReportContract = (name: string, contract: ReportContract) =>
  api.put<{ ok: boolean; contract: ReportContract }>(
    `/apps/${name}/report-contracts/${encodeURIComponent(contract.id)}`,
    contract,
  ).then(r => r.data.contract)
export const previewDelivery = (
  name: string,
  delivery: Record<string, unknown>,
  eventId = '',
  send = false,
) => api.post(`/apps/${name}/delivery-preview`, {
  delivery, event_id: eventId, send,
}).then(r => r.data as { preview: Record<string, unknown>; test?: Record<string, unknown> })
export const fetchOtaConfig = (name: string) =>
  api.get<OtaConfig>(`/apps/${name}/ota-config`).then(r => r.data)
export const saveOtaConfig = (name: string, cfg: OtaConfig) =>
  api.post(`/apps/${name}/ota-config`, cfg).then(r => r.data)

// ── Live MJPEG stream URL (用于 <img src>) ─────────────────────────────────
// <img> 无法携带 Authorization 头，token 走查询参数 (后端 auth_middleware 已放行)。
// 默认 15 FPS；需要对比时可改成 20。传入 0 表示不主动限帧。
export const streamUrl = (name: string, fps = 25): string => {
  const token = useAuthStore.getState().token ?? ''
  return `/api/apps/${encodeURIComponent(name)}/stream?fps=${Math.max(0, Math.floor(fps))}&token=${encodeURIComponent(token)}`
}

export interface StreamHealth {
  active: boolean
  last_data_age_ms: number | null
  restart_count: number
}

export const fetchStreamHealth = (name: string) =>
  api.get<StreamHealth>(`/apps/${name}/stream-health`).then(r => r.data)

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
  bound_config: string | null   // OTA 当前绑定的视觉启动配置
  working_dir: string | null    // 单元的 WorkingDirectory（用于判断是否失效）
  path_ok: boolean              // WorkingDirectory 是否真实存在；false=失效单元，需重装修正
  autostart: boolean            // 用户是否勾选“开机自启”
  desired_running: boolean      // 用户最后一次操作是否要求保持运行
}

export const fetchServices = () => api.get<ServiceInfo[]>('/services').then(r => r.data)

export const controlService = (key: string, action: 'start' | 'stop' | 'restart') =>
  api.post(`/services/${key}/${action}`).then(r => r.data)

export const setServiceAutostart = (key: string, enabled: boolean) =>
  api.post(`/services/${key}/autostart`, { enabled }).then(r => r.data)

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

export const deleteApp = (name: string) =>
  api.delete(`/apps/${name}`).then(r => r.data)

// ── 本地事件发件箱 ──────────────────────────────────────────────────────────
export interface EventRecord {
  id: string
  channel_id: number | null
  event_type: string
  required_media: string[]
  trigger_count?: number
  snap_time: string | number
  created_unix_sec: number
  has_raw_image: boolean
  has_annotated_image: boolean
  has_video: boolean
  message?: string
  state: string
  media_statuses?: Record<string, { status: string; error?: string }>
  total_bytes?: number
  deliveries: Array<{
    id?: string; media?: string[]; profile_id?: string; contract_id?: string; status?: string
    attempts?: number; last_error?: string
  }>
}
export interface RecordsResp {
  records: EventRecord[]
  count: number
  total_bytes: number
  cap_bytes: number
}

export const fetchRecords = (name: string, limit = 500) =>
  api.get<RecordsResp>(`/apps/${name}/records`, { params: { limit } }).then(r => r.data)

export interface RecordJsonResponse {
  schema_version: number
  event: Record<string, unknown>
  source: Record<string, unknown>
  fields: Record<string, unknown>
  media: Record<string, string>
  media_statuses: Record<string, { status: string; error?: string }>
  deliveries: Array<Record<string, unknown>>
}

export const fetchRecordJson = (name: string, id: string) =>
  api.get<RecordJsonResponse>(
    `/apps/${encodeURIComponent(name)}/records/${encodeURIComponent(id)}/json`,
  ).then(r => r.data)

// <img> 无法带 Authorization 头，token 走查询参数（后端 auth_middleware 已放行）
export const recordImageUrl = (name: string, id: string, raw = false): string => {
  const token = useAuthStore.getState().token ?? ''
  return `/api/apps/${encodeURIComponent(name)}/records/${encodeURIComponent(id)}/image`
    + `?raw=${raw ? 1 : 0}&token=${encodeURIComponent(token)}`
}

export const recordVideoUrl = (name: string, id: string): string => {
  const token = useAuthStore.getState().token ?? ''
  return `/api/apps/${encodeURIComponent(name)}/records/${encodeURIComponent(id)}/video`
    + `?token=${encodeURIComponent(token)}`
}

export const retryRecord = (name: string, id: string) =>
  api.post(`/apps/${name}/records/${id}/retry`).then(r => r.data)

export const deleteRecord = (name: string, id: string) =>
  api.delete(`/apps/${name}/records/${id}`).then(r => r.data)

export const deleteAllRecords = (name: string) =>
  api.delete(`/apps/${name}/records`).then(r => r.data)
