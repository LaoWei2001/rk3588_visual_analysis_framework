import { api } from './http'

// ── 设备系统设置 ────────────────────────────────────────────────────────────
export interface DailyRebootSettings {
  enabled: boolean
  configured_enabled?: boolean
  time: string
  installed: boolean
  active: boolean
  unit_enabled: boolean
  next_run: string | null
  timezone: string
  current_time: string
  current_time_epoch_ms: number
  utc_offset_minutes: number
  ntp_enabled?: boolean | null
  ntp_synchronized?: boolean | null
  can_ntp?: boolean | null
  time_error?: string | null
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

export const saveSystemTime = (value: { timezone: string; local_time?: string; epoch_ms?: number; disable_ntp: boolean }) =>
  api.put<DailyRebootSettings & { ok: boolean }>('/system/time', value).then(r => r.data)
