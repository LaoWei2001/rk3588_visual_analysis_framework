import { api } from './http'

// ── 板端后台服务 (systemd 单元: OTA / 告警上报 / GPIO 电平保持) ─────────────
export interface ServiceInfo {
  key: string
  label: string
  unit: string
  scope: 'app' | 'system'       // 应用绑定服务或独立板级服务
  installed: boolean
  active_state: string          // active / inactive / failed / activating / unknown
  sub_state: string
  enabled: boolean
  uptime_seconds: number | null
  n_restarts: number | null
  bound_app: string | null      // 单元当前绑定到哪个 App 的 services/
  bound_config: string | null   // OTA 当前绑定的视觉启动配置
  working_dir: string | null    // 单元的 WorkingDirectory（用于判断是否失效）
  path_ok: boolean              // 应用工作目录或系统 unit 文件是否真实存在
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
