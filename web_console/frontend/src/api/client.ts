import axios from 'axios'
import { useAuthStore } from '../store/authStore'

const api = axios.create({ baseURL: '/api' })

export const apiErrorMessage = (error: unknown): string => {
  if (axios.isAxiosError(error)) {
    const detail = error.response?.data?.detail ?? error.response?.data?.message
    return typeof detail === 'string' ? detail : error.message
  }
  return error instanceof Error ? error.message : String(error)
}

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
export interface LogicOutputDef {
  key: string
  type: 'string' | 'number' | 'integer' | 'boolean' | 'json'
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
  outputs?: LogicOutputDef[]                               // 通道 logic 向全局 logic 公开的同帧变量
}

export interface AppLogics {
  channel_logics: LogicDef[]
  global_logics:  LogicDef[]
  model_types:    string[]
  error?: string
}

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

// ── 当前程序包的投递连接、契约模板与 OTA 配置 ──
export interface DeliveryConnection {
  adapter: string
  [key: string]: unknown
}
export interface DeliveryConnectionsConfig {
  connections: Record<string, DeliveryConnection>
}
export interface AdapterConnectionField {
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
  connection_fields: AdapterConnectionField[]
  transforms: string[]
}
export interface ReportContract {
  id: string
  version: number
  label: string
  description?: string
  owner_logic?: string
  event_types: string[]
  adapter: string
  media: Array<'annotated_image' | 'raw_image' | 'video'>
  mapping: Array<{
    source: string
    target: string
    value?: unknown
    type?: string
    transform?: string
    location?: 'body' | 'query' | 'form' | 'header' | 'file'
    file_mode?: 'single' | 'list'
    required?: boolean
  }>
  request?: Record<string, unknown>
  success?: Record<string, unknown>
  origin?: 'package' | 'custom'
  revision: string
}
export interface OtaConfig {
  platform_ws_host: string
}

export const fetchConnections = (name: string) =>
  api.get<DeliveryConnectionsConfig>(`/apps/${name}/connections`).then(r => r.data)
export const saveConnections = (name: string, cfg: DeliveryConnectionsConfig) =>
  api.put(`/apps/${name}/connections`, cfg).then(r => r.data)
export const fetchDeliveryAdapters = (name: string) =>
  api.get<{ adapters: DeliveryAdapterDef[] }>(`/apps/${name}/delivery-adapters`).then(r => r.data.adapters)
export const fetchReportContracts = (name: string) =>
  api.get<{ contracts: ReportContract[] }>(`/apps/${name}/report-contracts`).then(r => r.data.contracts)
export const saveReportContract = (name: string, contract: ReportContract) =>
  api.put<{ ok: boolean; contract: ReportContract }>(
    `/apps/${name}/report-contracts/${encodeURIComponent(contract.id)}`,
    contract,
  ).then(r => r.data.contract)
export const deleteReportContract = (name: string, contractId: string) =>
  api.delete(`/apps/${name}/report-contracts/${encodeURIComponent(contractId)}`).then(r => r.data)
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

// ── Live H264 fMP4 stream URL (由 MSE fetch，Authorization 走请求头) ──────────
export const streamUrl = (name: string): string =>
  `/api/apps/${encodeURIComponent(name)}/stream`

// ── 设备系统设置 ────────────────────────────────────────────────────────────
export interface DailyRebootSettings {
  enabled: boolean
  time: string
  installed: boolean
  active: boolean
  unit_enabled: boolean
  next_run: string | null
  timezone: string
  current_time: string
  current_time_epoch_ms: number
  utc_offset_minutes: number
  error: string | null
}

export const fetchDailyRebootSettings = () =>
  api.get<DailyRebootSettings>('/system/daily-reboot').then(r => r.data)

export const saveDailyRebootSettings = (enabled: boolean, time: string) =>
  api.put<DailyRebootSettings & { ok: boolean }>('/system/daily-reboot', { enabled, time }).then(r => r.data)

export const fetchSystemTimezones = () =>
  api.get<{ timezones: string[] }>('/system/timezones').then(r => r.data.timezones)

export const saveSystemTimezone = (timezone: string) =>
  api.put<DailyRebootSettings & { ok: boolean }>('/system/timezone', { timezone }).then(r => r.data)

export interface StorageCleanupResult {
  finished_unix_ms: number
  deleted_count: number
  deleted_bytes: number
  skipped_active: number
}

export interface RootCleanupTarget {
  key: string
  label: string
  description: string
  path: string
  exists: boolean
  bytes: number
}

export interface RootCleanupResult {
  finished_unix_ms: number
  deleted_count: number
  freed_bytes: number
  deleted: Array<{ key: string; path: string; bytes: number }>
  errors: Array<{ key: string; path: string; error: string }>
}

export interface StorageSettings {
  auto_cleanup: boolean
  retention_days: number
  max_event_store_gb: number
  min_free_gb: number
  storage_path: string
  total_bytes: number
  used_bytes: number
  free_bytes: number
  used_percent: number
  event_bytes: number
  event_count: number
  root_cleanup_targets: RootCleanupTarget[]
  last_cleanup: StorageCleanupResult | null
}

export const fetchStorageSettings = () =>
  api.get<StorageSettings>('/system/storage').then(r => r.data)

export const saveStorageSettings = (settings: Pick<StorageSettings,
  'auto_cleanup' | 'retention_days' | 'max_event_store_gb' | 'min_free_gb'>) =>
  api.put<StorageSettings & { ok: boolean }>('/system/storage', settings).then(r => r.data)

export const cleanupStorageNow = () =>
  api.post<StorageSettings & { ok: boolean; cleanup: StorageCleanupResult }>('/system/storage/cleanup').then(r => r.data)

export const cleanupRootStorageTargets = (targets: string[]) =>
  api.post<StorageSettings & { ok: boolean; root_cleanup: RootCleanupResult }>(
    '/system/storage/root-cleanup', { targets },
  ).then(r => r.data)

export interface NetworkInterfaceInfo {
  device: string
  type: 'ethernet' | 'wifi'
  state: string
  connection: string
  connection_uuid: string | null
  mac: string
  addresses: string[]
  gateway: string
  dns: string[]
  ipv4_method: string
  ssid: string
  configurable: boolean
}

export interface NetworkConnectionInfo {
  name: string
  uuid: string
  type: 'ethernet' | 'wifi'
  device: string
  autoconnect: boolean
  active: boolean
  ipv4_method: string
  addresses: string[]
  gateway: string
  dns: string[]
  ssid: string
  security: string
}

export type NetworkTransactionStatus =
  | 'scheduled' | 'activating' | 'awaiting_confirmation' | 'committing'
  | 'rolling_back' | 'confirmed' | 'rolled_back' | 'rollback_failed'

export interface NetworkTransaction {
  id: string
  status: NetworkTransactionStatus
  device: string
  kind: 'staged_profile' | 'saved_profile'
  old_uuid: string | null
  new_uuid: string | null
  profile_name: string
  target_addresses: string[]
  deadline: number
  remaining_seconds: number
  error: string | null
}

export interface WifiNetworkInfo {
  in_use: boolean
  ssid: string
  signal: number
  security: string
}

export interface NetworkSettings {
  hostname: string
  manager: string
  config_supported: boolean
  rollback_supported: boolean
  interfaces: NetworkInterfaceInfo[]
  connections: NetworkConnectionInfo[]
  pending_transaction: NetworkTransaction | null
  error: string | null
}

export const fetchNetworkSettings = () =>
  api.get<NetworkSettings>('/system/network').then(r => r.data)

export const saveDeviceHostname = (hostname: string) =>
  api.put<{ ok: boolean; hostname: string }>('/system/network/hostname', { hostname }).then(r => r.data)

export interface NetworkApplyConfig {
  device: string
  type: 'ethernet' | 'wifi'
  connection_uuid?: string | null
  profile_name: string
  method: 'auto' | 'manual'
  address: string
  gateway: string
  dns: string[]
  ssid: string
  wifi_security: 'wpa-psk' | 'sae' | 'open'
  wifi_password: string
  rollback_seconds?: number
}

export const startNetworkChange = (config: NetworkApplyConfig) =>
  api.post<{ ok: boolean; transaction: NetworkTransaction }>('/system/network/changes', config).then(r => r.data)

export const scanWifiNetworks = (device: string) =>
  api.post<{ device: string; networks: WifiNetworkInfo[] }>('/system/network/wifi/scan', null, { params: { device } }).then(r => r.data)

export const activateNetworkConnection = (connection_uuid: string, device: string) =>
  api.post<{ ok: boolean; transaction: NetworkTransaction }>('/system/network/connections/activate', {
    connection_uuid, device, rollback_seconds: 60,
  }).then(r => r.data)

export const fetchNetworkTransaction = (id: string, baseUrl = '') => {
  if (!baseUrl) return api.get<NetworkTransaction>(`/system/network/transactions/${id}`).then(r => r.data)
  const token = useAuthStore.getState().token
  return axios.get<NetworkTransaction>(`${baseUrl}/api/system/network/transactions/${id}`, {
    headers: token ? { Authorization: `Bearer ${token}` } : undefined,
    timeout: 5000,
  }).then(r => r.data)
}

export const confirmNetworkTransaction = (id: string, baseUrl = '') => {
  const token = useAuthStore.getState().token
  const url = baseUrl
    ? `${baseUrl}/api/system/network/transactions/${id}/confirm`
    : `/api/system/network/transactions/${id}/confirm`
  return axios.post<NetworkTransaction>(url, null, {
    headers: token ? { Authorization: `Bearer ${token}` } : undefined,
    timeout: 12_000,
  }).then(r => r.data)
}

export const rollbackNetworkTransaction = (id: string) =>
  api.post<NetworkTransaction>(`/system/network/transactions/${id}/rollback`).then(r => r.data)

export const deleteNetworkConnection = (uuid: string) =>
  api.delete<{ ok: boolean }>(`/system/network/connections/${uuid}`).then(r => r.data)

export const pingNetworkTarget = (target: string) =>
  api.post<{ target: string; reachable: boolean; detail: string }>('/system/network/ping', { target }).then(r => r.data)

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
    id?: string; media?: string[]; connection_id?: string; contract_id?: string
    contract_revision?: string; status?: string
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

export interface DeliveryHistoryRecord {
  event_id: string
  event_type: string
  snap_time: string
  channel_id: number | null
  connection_id: string
  contract_id: string
  contract_revision: string
  status: string
  attempts: number
  http_status: number
  detail: string
  response: unknown
  updated_unix_ms: number
}
export const fetchDeliveryHistory = (name: string, limit = 500) =>
  api.get<{ records: DeliveryHistoryRecord[]; count: number }>(
    `/apps/${name}/delivery-history`, { params: { limit } },
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
