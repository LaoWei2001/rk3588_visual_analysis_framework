import { create } from 'zustand'
import type { AppAssets, DeliveryConnection } from '../api/client'
import { fetchAssets, fetchConnections } from '../api/client'

interface EditorState {
  appName: string
  assets: AppAssets
  assetsForApp: string
  assetsStatus: 'idle' | 'loading' | 'ready' | 'error'
  deliveryConnections: Record<string, DeliveryConnection>
  globalMaxFps: number
  dirty: boolean
  appIntegrationDirty: boolean
  setAppName: (name: string) => void
  setDeliveryConnections: (connections: Record<string, DeliveryConnection>) => void
  setGlobalMaxFps: (fps: number) => void
  setDirty: (dirty: boolean) => void
  setAppIntegrationDirty: (dirty: boolean) => void
  loadAssets: (name: string) => Promise<void>
  loadDeliveryConnections: (name: string) => Promise<void>
}

const EMPTY_ASSETS: AppAssets = { models: [], labels: [], videos: [] }

export const useEditorStore = create<EditorState>((set, get) => ({
  appName: '',
  assets: EMPTY_ASSETS,
  assetsForApp: '',
  assetsStatus: 'idle',
  deliveryConnections: {},
  globalMaxFps: 25,
  dirty: false,
  appIntegrationDirty: false,
  setAppName: appName => set(state => state.appName === appName
    ? { appName }
    : { appName, assets: EMPTY_ASSETS, assetsForApp: '', assetsStatus: 'idle' }),
  setDeliveryConnections: deliveryConnections => set({ deliveryConnections }),
  setGlobalMaxFps: fps => set({ globalMaxFps: fps > 0 ? fps : 25 }),
  setDirty: dirty => set({ dirty }),
  setAppIntegrationDirty: appIntegrationDirty => set({ appIntegrationDirty }),
  loadAssets: async name => {
    set({ assetsStatus: 'loading' })
    try {
      const assets = await fetchAssets(name)
      if (get().appName === name) set({ assets, assetsForApp: name, assetsStatus: 'ready' })
    } catch {
      if (get().appName === name) set({ assets: EMPTY_ASSETS, assetsForApp: name, assetsStatus: 'error' })
    }
  },
  loadDeliveryConnections: async name => {
    try {
      const config = await fetchConnections(name)
      set({ deliveryConnections: config.connections ?? {} })
    } catch {
      set({ deliveryConnections: {} })
    }
  },
}))
