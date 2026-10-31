import { api } from './http'
import axios from 'axios'
import { useAuthStore } from '../store/authStore'

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

export const deleteNetworkConnection = (uuid: string) =>
  api.delete<{ ok: boolean }>(`/system/network/connections/${uuid}`).then(r => r.data)

export const pingNetworkTarget = (target: string) =>
  api.post<{ target: string; reachable: boolean; detail: string }>('/system/network/ping', { target }).then(r => r.data)
