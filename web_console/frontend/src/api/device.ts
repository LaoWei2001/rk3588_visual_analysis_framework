import { api } from './http'
import type { RecoveryHistorySnapshot } from './recovery'

export interface DeviceInformation {
  updated_at: number
  performance?: PerformanceSnapshot | null
  system: {
    hostname?: string; os_name?: string; kernel?: string; soc?: string | null; serial?: string | null
    uptime_sec?: number | null; boot_time?: number | null; current_time?: string; timezone?: string | null
    application_directory?: string; ntp_enabled?: boolean | null; ntp_synchronized?: boolean | null
  }
  cpu: { architecture?: string; cores?: number | null; usage_percent?: number | null; usage_sampling?: boolean; max_mhz?: number | null; load_average?: number[] }
  memory: {
    total_bytes?: number; available_bytes?: number; used_bytes?: number; usage_percent?: number
    available_estimated?: boolean; swap_total_bytes?: number; swap_used_bytes?: number
  }
  storage: { mount: string; source: string; filesystem: string; total_bytes: number; used_bytes: number
    available_bytes: number; usage_percent: number; readonly: boolean; application_data: boolean }[]
  network: { interfaces: { name: string; kind: string; link_up: boolean; mac: string; addresses: string[]
    speed_mbps: number | null; gateways: string[] }[]; dns: string[] }
  temperatures: { label: string; celsius: number; critical_celsius: number | null; critical_reached?: boolean }[]
  accelerators: { kind: string; driver: string | null; registered: boolean; current_mhz: number | null }[]
  services: { unit: string; label: string; load_state: string; active_state: string; sub_state: string; autostart: boolean | null }[]
  application: { state: string; name?: string; config?: string }
  diagnostics: RecoveryHistorySnapshot['diagnostics']
  observer: RecoveryHistorySnapshot['observer']
  errors: string[]
}
export interface PerformanceDevice {
  id: string; kind: 'CPU' | 'GPU' | 'NPU'; label: string; cores: string
  current_mhz: number | null; min_mhz: number | null; max_mhz: number | null
  available_mhz: number[]; governor: string | null; usage_percent: number | null
  usage_source: string | null; core_loads: { core: number; usage_percent: number }[]
  temperature_celsius: number | null; driver: string | null; editable: boolean
}
export interface PerformanceTrip {
  id: string; zone: string; kind: 'passive' | 'hot' | 'critical'; celsius: number
  hysteresis_celsius: number; default_celsius: number | null; ceiling_celsius: number | null
  editable: boolean; sensor_celsius: number | null
}
export interface PerformanceSnapshot {
  sampled_at: number; devices: PerformanceDevice[]; thermal_trips: PerformanceTrip[]
  revision: string; saved: boolean; applied: boolean; profile_matches: boolean
  error: string | null; notice: string | null
}
export const fetchDeviceInformation = (signal?: AbortSignal) => api.get<DeviceInformation>('/system/device-info', { signal }).then(r => r.data)
export const savePerformance = (limits: Record<string, number>, revision: string) =>
  api.put<PerformanceSnapshot>('/system/device-info/performance', { limits, revision }).then(r => r.data)
export const resetPerformance = (revision: string) =>
  api.post<PerformanceSnapshot>('/system/device-info/performance/reset', { revision }).then(r => r.data)
