import { create } from 'zustand'
import type { AppAssets, DeliveryConnection } from '../api/client'
import { fetchAssets, fetchConnections } from '../api/client'

interface EditorState {
  appName: string
  assets: AppAssets
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

export const useEditorStore = create<EditorState>((set) => ({
  appName: '',
  assets: EMPTY_ASSETS,
  deliveryConnections: {},
  globalMaxFps: 25,
  dirty: false,
  appIntegrationDirty: false,
  setAppName: appName => set({ appName }),
  setDeliveryConnections: deliveryConnections => set({ deliveryConnections }),
  setGlobalMaxFps: fps => set({ globalMaxFps: fps > 0 ? fps : 25 }),
  setDirty: dirty => set({ dirty }),
  setAppIntegrationDirty: appIntegrationDirty => set({ appIntegrationDirty }),
  loadAssets: async name => {
    try {
      set({ assets: await fetchAssets(name) })
    } catch {
      set({ assets: EMPTY_ASSETS })
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
