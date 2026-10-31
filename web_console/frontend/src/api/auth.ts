import { api, sharedRead } from './http'

// ── Auth ──────────────────────────────────────────────────────────────────
export const apiLogin = (username: string, password: string) =>
  api.post<{ token: string; username: string }>('/auth/login', { username, password }).then(r => r.data)

export const apiLogout = () =>
  api.post('/auth/logout').then(r => r.data)

// WebSocket-only views also need an HTTP check: failed handshakes usually expose
// only code 1006, which cannot distinguish an expired login from a network outage.
export const apiCheckSession = () => sharedRead('auth/me', () =>
  api.get<{ username: string }>('/auth/me', { timeout: 5000 }).then(r => r.data))
