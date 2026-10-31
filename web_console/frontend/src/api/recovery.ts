import { api, sharedRead } from './http'

export interface RecoveryPolicy {
  manager_enabled: boolean
  app_restart_enabled: boolean
  app_stall_enabled: boolean
  restart_delay_sec: number
  task_timeout_sec: number
  startup_timeout_sec: number
  kernel_reboot_enabled: boolean
  kernel_hang_sec: number
  reboot_cooldown_sec: number
  max_reboots_per_day: number
}
export interface SystemRecoveryPolicy {
  hardware_enabled: boolean
  panic_enabled: boolean
  watchdog_timeout_sec: number
  reboot_timeout_sec: number
  panic_timeout_sec: number
}
export interface SystemRecoverySettings {
  configured?: boolean
  policy: SystemRecoveryPolicy | null; revision: string | null; error: string | null
  can_enable_hardware: boolean; can_set_panic: boolean; hardware_unavailable_reason: string | null
  hardware_pending: boolean; hardware_apply_error: string | null; panic_in_sync: boolean
}
export interface RecoveryStatus {
  history?: RecoveryHistorySnapshot
  system?: SystemRecoverySettings
  policy: RecoveryPolicy
  revision: string
  policy_error: string | null
  agent: { installed: boolean; active: boolean; enabled: boolean; state: string; heartbeat_fresh: boolean; error: string | null }
  monitor: { installed: boolean; active: boolean; error: string | null }
  hardware: {
    enabled: boolean; device?: string | null; driver?: string | null
    state?: string; label?: string; summary?: string; next_steps?: string[]
    diagnostics?: {
      devices: { device: string; driver: string; node_present: boolean; state: string | null; identity: string | null; manager_owned: boolean }[]
      device_tree: { path: string; compatible: string[]; status: string; disabled_by: string | null }[]
      kernel_config: Record<string, string>; registered_drivers: string[]; errors: string[]
    }
    configured_device?: string | null; reboot_required?: boolean
    runtime_timeout: string; reboot_timeout: string; panic_timeout_sec?: number | null
    verification_state?: 'unverified' | 'failed' | 'passed'; verification_message?: string
    error: string | null
  }
  runtime: { phase: string; error: string | null; retry_in_sec: number; updated_at: number } | null
  current: { app: string; pid: number | null; config: string; restart_count?: number; watchdog_active?: boolean; restart_enabled?: boolean | null; recovery_state?: string } | null
  boot_target: { app: string; config: string } | null
  application_pending: boolean
  events: { time: number; kind: string; message: string }[]
}
export const fetchRecoveryStatus = () => sharedRead('recovery', () => api.get<RecoveryStatus>('/system/recovery').then(r => r.data))
export const saveSystemRecovery = (policy: SystemRecoveryPolicy, revision: string) =>
  api.put<RecoveryStatus>('/system/recovery/system', { policy, revision }).then(r => r.data)
export const saveRecoveryPolicy = (policy: RecoveryPolicy, revision: string) =>
  api.put<RecoveryStatus>('/system/recovery', { policy, revision }).then(r => r.data)
export const controlRuntimeAgent = (action: 'start' | 'stop' | 'restart') =>
  api.post<RecoveryStatus>(`/system/recovery/agent/${action}`).then(r => r.data)
export const applyRecoveryPolicy = () => api.post<RecoveryStatus>('/system/recovery/apply').then(r => r.data)
export type ApplicationRecoveryPolicy = Pick<RecoveryPolicy, 'app_restart_enabled' | 'app_stall_enabled' | 'restart_delay_sec' | 'task_timeout_sec' | 'startup_timeout_sec'>
export const saveApplicationRecovery = (policy: ApplicationRecoveryPolicy, revision: string) =>
  api.put<RecoveryStatus>('/system/recovery/application', { policy, revision }).then(r => r.data)
export const applyApplicationRecovery = (revision: string, app: string) =>
  api.post<RecoveryStatus>('/system/recovery/application/apply', { revision, app }).then(r => r.data)

export type NetworkRecoveryTarget = { host: string; method: 'ping' } | { host: string; method?: 'tcp'; port: number }
export interface NetworkRecoveryPolicy {
  enabled: boolean
  interface: string
  targets: NetworkRecoveryTarget[]
  failure_timeout_sec: number
  recovery_wait_sec: number
  action_cooldown_sec: number
  allow_manager_restart: boolean
  allow_reboot: boolean
  reboot_after_sec: number
  reboot_cooldown_sec: number
  max_reboots_per_day: number
}
export interface NetworkRecoveryStatus {
  history?: RecoveryHistorySnapshot
  requirements?: { ready: boolean; missing_tools: string[]; message: string | null }
  policy: NetworkRecoveryPolicy; revision: string; policy_error: string | null
  service: { installed: boolean; active: boolean; enabled: boolean; heartbeat_fresh: boolean; error: string | null }
  interfaces: { name: string; type: string; state: string }[]
  interface_error: string | null; policy_pending: boolean
  runtime: {
    phase: string; message: string; error?: string; armed?: boolean; fault_age_sec?: number
    observation?: {
      interface: string; gateway: string; gateway_reachable: boolean | null; addresses: string[]
      targets: (NetworkRecoveryTarget & { reachable: boolean })[]
    }
  } | null
  events: { time: number; kind: string; message: string }[]
}
export interface RecoveryHistoryEvent {
  id: string; time: number; updated_at: number
  category: 'system' | 'network' | 'application'; kind: string
  reason: string; action: string; result: 'pending' | 'performed' | 'recovered' | 'failed' | 'unknown'
  detail: string; source: string; evidence: string; time_basis: string; boot_id: string
  fault_recovery?: boolean
  stages?: {
    id: string; kind: string; time: number; time_basis: string; action: string
    result: RecoveryHistoryEvent['result']; detail: string; source: string
  }[]
  diagnosis?: {
    confidence: 'confirmed' | 'probable' | 'unknown'
    findings: { code: string; summary: string }[]; limitations: string[]
    recovery?: {
      confidence: 'confirmed' | 'probable' | 'unknown'
      method: string; basis: string[]; limitations: string[]
    }
  }
}
export interface RecoveryHistorySnapshot {
  events: RecoveryHistoryEvent[]; keep_per_category: number; keep_operations?: number; error: string | null
  observer?: { boot_id?: string; updated_at?: number; error?: string | null; diagnostic_error?: string | null }
  diagnostics?: {
    board?: {
      model?: string; compatible?: string; kernel?: string; limitations: string[]
      watchdogs?: { device: string; identity?: string; bootstatus?: string }[]
      reset_sources?: { source: string; value: string }[]
      pstore?: boolean; psi?: { cpu: boolean; memory: boolean; io: boolean }
    } | null
    sample_time?: number; errors?: string[]; error?: string | null
  }
}
export const fetchRecoveryHistory = () => api.get<RecoveryHistorySnapshot>('/system/recovery/history').then(r => r.data)
export const fetchNetworkRecovery = () => api.get<NetworkRecoveryStatus>('/system/recovery/network').then(r => r.data)
export const saveNetworkRecovery = (policy: NetworkRecoveryPolicy, revision: string) =>
  api.put<NetworkRecoveryStatus>('/system/recovery/network', { policy, revision }).then(r => r.data)
