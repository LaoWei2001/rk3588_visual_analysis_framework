import { useEffect, useState } from 'react'
import type { ImageSettings } from '../../api/imageInference'
import { useAuthStore } from '../../store/authStore'
import type { SourceChoice } from './SourcePicker'

interface Draft {
  settings: ImageSettings
  source: SourceChoice
  selectedApps: Record<string, string>
}
// File handles survive page navigation in this tab, but cannot be restored from JSON after a reload.
const fileSelections = new Map<string, File[]>()
const drafts = new Map<string, Draft>()
const defaultDraft = (): Draft => ({
  settings: { models: [{ id: 'model_1', model_type: 'yolov8_det', model_path: '', label_path: '',
    obj_thresh: .3, nms_thresh: .45, npu_core: -1, detect_classes: [] }] },
  source: { kind: 'upload', path: '', recursive: false }, selectedApps: {},
})

function restore(key: string): Draft {
  const cached = drafts.get(key)
  if (cached) return cached
  const fallback = defaultDraft()
  try {
    const value = JSON.parse(localStorage.getItem(key) ?? 'null')
    if (!value || !Array.isArray(value.settings?.models) || !value.settings.models.length ||
      !value.settings.models.every((model: Record<string, unknown>) => model &&
        ['id', 'model_type', 'model_path', 'label_path'].every(k => typeof model[k] === 'string') &&
        ['obj_thresh', 'nms_thresh'].every(k => typeof model[k] === 'number' || model[k] === null) &&
        typeof model.npu_core === 'number' && Array.isArray(model.detect_classes) &&
        model.detect_classes.every(c => typeof c === 'string'))) return fallback
    return {
      settings: value.settings,
      source: ['upload', 'device'].includes(value.source?.kind) && typeof value.source.path === 'string' &&
        typeof value.source.recursive === 'boolean' ? value.source : fallback.source,
      selectedApps: value.selectedApps && typeof value.selectedApps === 'object' &&
        !Array.isArray(value.selectedApps) && Object.values(value.selectedApps).every(v => typeof v === 'string')
        ? value.selectedApps : {},
    }
  } catch { return fallback }
}

export default function useInferenceDraft() {
  const [key] = useState(() => `rk3588.imageInference.draft.v1:${useAuthStore.getState().username ?? ''}`)
  const [initial] = useState(() => restore(key))
  const [settings, setSettings] = useState(initial.settings)
  const [source, setSource] = useState(initial.source)
  const [selectedApps, setSelectedApps] = useState(initial.selectedApps)
  const [files, setFiles] = useState<File[]>(() => fileSelections.get(key) ?? [])
  useEffect(() => {
    drafts.set(key, { settings, source, selectedApps })
    try { localStorage.setItem(key, JSON.stringify({ settings, source, selectedApps })) }
    catch { /* In-tab navigation still retains the draft when storage is disabled. */ }
  }, [key, settings, source, selectedApps])
  useEffect(() => { fileSelections.set(key, files) }, [key, files])
  return { settings, setSettings, source, setSource, selectedApps, setSelectedApps, files, setFiles }
}
