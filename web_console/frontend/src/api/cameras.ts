import { api } from './http'

// ── 直连网络摄像头 ────────────────────────────────────────────────────────
export interface CameraInterfaceInfo {
  device: string
  ifindex: number
  mac: string
  operstate: string
  link_up: boolean
  speed_mbps: number | null
  addresses: string[]
  has_default_route: boolean
  default_gateway: string
  configured: boolean
}

export interface DiscoveredCameraInfo {
  ip: string
  mac: string
  model: string
  serial: string
  subnet_mask: string
  prefix_length: number | null
  gateway: string
  http_port: number
  rtsp_port: number
  http_port_inferred: boolean
  rtsp_port_inferred: boolean
  hikvision: boolean
  source: string
}

export interface CameraConfigurationInput {
  interface: string
  camera_ip: string
  prefix_length: number
  camera_mac: string
  model: string
  serial: string
  http_port: number
  rtsp_port: number
  http_port_inferred: boolean
  rtsp_port_inferred: boolean
}

export interface CameraNetworkConflict {
  interface: string
  network: string
  address: string
}

export interface CameraNetworkPlan {
  interface: string
  camera_ip: string
  camera_prefix_length: number
  camera_network: string
  camera_mac: string
  model: string
  serial: string
  http_port: number
  rtsp_port: number
  http_port_inferred: boolean
  rtsp_port_inferred: boolean
  local_ip: string
  local_prefix_length: number
  isolation_mode: 'host-route'
  conflicts: CameraNetworkConflict[]
  warnings: string[]
  address_preexisting: boolean
  route_preexisting: boolean
}

export interface CameraConfiguration extends CameraNetworkPlan {
  version: number
  address_owned: boolean
  route_owned: boolean
  applied_at: number
  restored_at?: number
}

export interface CameraStatusInfo {
  checked_at: number
  link_up: boolean
  arp_reachable: boolean
  arp_mac: string
  http_reachable: boolean
  http_status: number
  rtsp_reachable: boolean
  rtsp_status: number
  mac_matches: boolean | null
}

export interface CameraSettingsSnapshot {
  interfaces: CameraInterfaceInfo[]
  configuration: CameraConfiguration | null
  status: CameraStatusInfo | null
  status_error?: string | null
  error: string | null
}

export interface CameraWebProxySession {
  port: number
  expires_at: number
  allowed_client_ip: string
  fallback_port: boolean
}

export const fetchCameraSettings = (includeStatus = true) =>
  api.get<CameraSettingsSnapshot>('/system/camera', {
    params: { include_status: includeStatus },
  }).then(r => r.data)

export const discoverCameras = (interfaceName: string, timeoutMs = 3000) =>
  api.post<{ interface: string; cameras: DiscoveredCameraInfo[] }>('/system/camera/discover', {
    interface: interfaceName, timeout_ms: timeoutMs,
  }).then(r => r.data)

export const planCameraConfiguration = (configuration: CameraConfigurationInput) =>
  api.post<CameraNetworkPlan>('/system/camera/plan', configuration).then(r => r.data)

export const applyCameraConfiguration = (configuration: CameraConfigurationInput) =>
  api.put<{
    ok: boolean
    configuration: CameraConfiguration
    status: CameraStatusInfo | null
    status_error: string | null
  }>('/system/camera/config', configuration).then(r => r.data)

export const removeCameraConfiguration = () =>
  api.delete<{ ok: boolean }>('/system/camera/config').then(r => r.data)

export const fetchCameraStatus = () =>
  api.get<{ status: CameraStatusInfo | null }>('/system/camera/status').then(r => r.data)

export const openCameraWebProxy = () =>
  api.post<{ ok: boolean } & CameraWebProxySession>('/system/camera/web-session').then(r => r.data)
