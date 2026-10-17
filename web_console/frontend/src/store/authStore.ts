import { create } from 'zustand'
import { persist } from 'zustand/middleware'
import { useDatasetStore } from './datasetStore'

interface AuthState {
  token: string | null
  username: string | null
  setAuth: (token: string, username: string) => void
  clearAuth: () => void
}

export const useAuthStore = create<AuthState>()(
  persist(
    (set) => ({
      token:    null,
      username: null,
      setAuth:  (token, username) => {
        if (useAuthStore.getState().username !== username) useDatasetStore.getState().reset()
        set({ token, username })
      },
      clearAuth: () => {
        useDatasetStore.getState().reset()
        set({ token: null, username: null })
      },
    }),
    { name: 'rk3588-auth' }   // persisted in localStorage
  )
)
