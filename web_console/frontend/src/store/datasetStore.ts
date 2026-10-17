import {create} from 'zustand'
import {createJSONStorage, persist} from 'zustand/middleware'
import type {DatasetDraft} from '../components/DatasetTaskEditor'
import {removeLegacyImageQuality, type DatasetReceiverPlatform} from '../api/dataset'

interface DatasetState {
  app: string
  drafts: Record<string, DatasetDraft>
  receiverPlatform: DatasetReceiverPlatform
  setApp: (app: string) => void
  setDraft: (app: string, update: DatasetDraft | null | ((draft: DatasetDraft | null) => DatasetDraft | null)) => void
  setReceiverPlatform: (platform: DatasetReceiverPlatform) => void
  reset: () => void
}

const defaults = () => ({
  app: '', drafts: {} as Record<string, DatasetDraft>,
  receiverPlatform: (/Windows/i.test(navigator.userAgent) ? 'windows-amd64' : 'python') as DatasetReceiverPlatform,
})

// Keep drafts across route unmounts and refreshes, separately for each app.
// Closing the tab or logging out clears this browser session's drafts.
export const useDatasetStore = create<DatasetState>()(persist((set) => ({
  ...defaults(),
  setApp: app => set({app}),
  setReceiverPlatform: receiverPlatform => set({receiverPlatform}),
  setDraft: (app, update) => set(state => {
    const draft = typeof update === 'function' ? update(state.drafts[app] ?? null) : update
    const drafts = {...state.drafts}
    if (draft) drafts[app] = draft
    else delete drafts[app]
    return {drafts}
  }),
  reset: () => set(defaults()),
}), {
  name: 'rk3588-dataset-drafts',
  storage: createJSONStorage(() => sessionStorage),
  partialize: ({app, drafts, receiverPlatform}) => ({app, drafts, receiverPlatform}),
  merge: (persisted, current) => {
    const saved = persisted as Partial<DatasetState> | undefined
    const drafts = {...saved?.drafts}
    for (const [app, draft] of Object.entries(drafts)) {
      let original = draft.original
      try {original = JSON.stringify(removeLegacyImageQuality(JSON.parse(original)))} catch { /* retain dirty draft */ }
      drafts[app] = {...draft, config: removeLegacyImageQuality(draft.config), original}
    }
    return {...current, ...saved, drafts}
  },
}))
