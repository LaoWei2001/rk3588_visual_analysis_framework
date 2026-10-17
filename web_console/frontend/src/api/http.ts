import axios from 'axios'
import { useAuthStore } from '../store/authStore'
import { invalidateConfigReads } from '../utils/configReadEpoch'

export const api = axios.create({ baseURL: '/api' })

const pendingReads = new Map<string, Promise<unknown>>()
// Mounted views may share a status endpoint. Share only in-flight reads, never
// cache a completed configuration snapshot or mix authenticated sessions.
export function sharedRead<T>(key: string, read: () => Promise<T>): Promise<T> {
  const scoped = `${useAuthStore.getState().token || ''}:${key}`
  const pending = pendingReads.get(scoped)
  if (pending) return pending as Promise<T>
  const request = read()
  pendingReads.set(scoped, request)
  const clear = () => { if (pendingReads.get(scoped) === request) pendingReads.delete(scoped) }
  void request.then(clear, clear)
  return request
}

export const apiErrorMessage = (error: unknown): string => {
  if (axios.isAxiosError(error)) {
    const detail = error.response?.data?.detail ?? error.response?.data?.message
    return typeof detail === 'string' ? detail : error.message
  }
  return error instanceof Error ? error.message : String(error)
}

// ── Request: attach Bearer token ──────────────────────────────────────────
api.interceptors.request.use(config => {
  if (!['get', 'head', 'options'].includes((config.method || 'get').toLowerCase())) {
    invalidateConfigReads()
    pendingReads.clear()
  }
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

