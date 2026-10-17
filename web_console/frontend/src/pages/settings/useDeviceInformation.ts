import { useEffect, useState } from 'react'
import { fetchDeviceInformation } from '../../api/client'
import type { DeviceInformation } from '../../api/client'

export function useDeviceInformation(intervalMs: number) {
  const [information, setInformation] = useState<DeviceInformation | null>(null)
  const [fetching, setFetching] = useState(true)
  const [error, setError] = useState('')

  useEffect(() => {
    let alive = true
    let pending = false
    const controller = new AbortController()
    const refresh = async () => {
      if (pending || document.visibilityState === 'hidden') return
      pending = true
      setFetching(true)
      try {
        const value = await fetchDeviceInformation(controller.signal)
        if (alive) {
          setInformation(previous => previous?.performance && value.performance && previous.performance.sampled_at > value.performance.sampled_at
            ? { ...value, performance: previous.performance } : value)
          setError('')
        }
      } catch (failure) {
        const value = failure as { response?: { data?: { detail?: string } }; message?: string }
        if (alive) setError(value.response?.data?.detail || value.message || '读取失败')
      } finally {
        pending = false
        if (alive) setFetching(false)
      }
    }
    void refresh()
    const timer = window.setInterval(() => { void refresh() }, intervalMs)
    const visible = () => { if (document.visibilityState === 'visible') void refresh() }
    document.addEventListener('visibilitychange', visible)
    return () => { alive = false; controller.abort(); window.clearInterval(timer); document.removeEventListener('visibilitychange', visible) }
  }, [intervalMs])

  return { information, setInformation, fetching, error }
}
