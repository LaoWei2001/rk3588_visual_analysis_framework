import { useEffect, useRef, useState, type Dispatch, type SetStateAction } from 'react'
import { configReadEpoch } from '../utils/configReadEpoch'

// Compare configuration content, independently of JSON object key order.
export const configSignature = (value: unknown): string => JSON.stringify(value, (_key, item) =>
  item && typeof item === 'object' && !Array.isArray(item)
    ? Object.fromEntries(Object.entries(item).sort(([a], [b]) => a.localeCompare(b))) : item)

/** One read at a time; late reads cannot cross a mutation, scope change, or unmount. */
export function useAutoSync<T>(read: () => Promise<T>, apply: (value: T) => void,
  { enabled = true, scope = '', immediate = true }: { enabled?: boolean; scope?: string; immediate?: boolean } = {}) {
  const latest = useRef({ read, apply, enabled })
  latest.current = { read, apply, enabled }
  const [error, setError] = useState('')
  useEffect(() => {
    if (!enabled) return
    let active = true
    let pending = false
    const refresh = async () => {
      if (!active || pending || !latest.current.enabled || document.visibilityState === 'hidden') return
      pending = true
      const epoch = configReadEpoch()
      try {
        const value = await latest.current.read()
        if (active && latest.current.enabled && epoch === configReadEpoch()) { latest.current.apply(value); setError('') }
      } catch (failure) {
        if (active && latest.current.enabled && epoch === configReadEpoch()) {
          const e = failure as { response?: { data?: { detail?: unknown } }; message?: string }
          setError(typeof e.response?.data?.detail === 'string' ? e.response.data.detail : e.message || String(failure))
        }
      } finally { pending = false }
    }
    if (immediate) void refresh()
    const timer = window.setInterval(() => void refresh(), 1000)
    const visible = () => { if (document.visibilityState === 'visible') void refresh() }
    document.addEventListener('visibilitychange', visible)
    return () => { active = false; window.clearInterval(timer); document.removeEventListener('visibilitychange', visible) }
  }, [enabled, scope, immediate])
  return error
}

/** Follow external changes while clean; retain the original baseline while editing. */
export function useSyncedDraft<T>(source: T | null, fallback: T, scope = ''):
  [T, Dispatch<SetStateAction<T>>, { dirty: boolean; conflict: boolean; reset: () => void; accept: (value: T) => void; isCurrent: (value: T) => boolean }] {
  const [state, setState] = useState({ value: source ?? fallback, baseline: source, scope })
  const signature = configSignature(source)
  useEffect(() => {
    setState(current => {
      if (current.scope !== scope) return { value: source ?? fallback, baseline: source, scope }
      if (source == null) return current
      if (current.baseline == null || configSignature(current.value) === configSignature(current.baseline)) {
        return { value: source, baseline: source, scope }
      }
      return current
    })
  }, [signature, scope]) // Configuration identity, rather than snapshot timestamps.
  const dirty = state.baseline != null && configSignature(state.value) !== configSignature(state.baseline)
  const conflict = dirty && source != null && configSignature(source) !== configSignature(state.baseline)
  const setValue: Dispatch<SetStateAction<T>> = update => setState(current => ({ ...current, baseline: current.baseline ?? current.value,
    value: typeof update === 'function' ? (update as (value: T) => T)(current.value) : update }))
  const reset = () => setState({ value: source ?? fallback, baseline: source, scope })
  const accept = (value: T) => setState({ value, baseline: value, scope })
  const isCurrent = (value: T) => state.baseline != null && configSignature(value) === configSignature(state.baseline)
  return [state.value, setValue, { dirty, conflict, reset, accept, isCurrent }]
}
