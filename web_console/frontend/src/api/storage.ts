import { api } from './http'

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

export const fetchStorageSettings = (details = true) =>
  api.get<StorageSettings>('/system/storage', { params: { details } }).then(r => r.data)

export const saveStorageSettings = (settings: Pick<StorageSettings,
  'auto_cleanup' | 'retention_days' | 'max_event_store_gb' | 'min_free_gb'>) =>
  api.put<StorageSettings & { ok: boolean }>('/system/storage', settings).then(r => r.data)

export const cleanupStorageNow = () =>
  api.post<StorageSettings & { ok: boolean; cleanup: StorageCleanupResult }>('/system/storage/cleanup').then(r => r.data)

export const cleanupRootStorageTargets = (targets: string[]) =>
  api.post<StorageSettings & { ok: boolean; root_cleanup: RootCleanupResult }>(
    '/system/storage/root-cleanup', { targets },
  ).then(r => r.data)
