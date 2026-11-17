import type { Edge, Node } from '@xyflow/react'
import type { ImageModel, ImageSettings } from '../../api/imageInference'

const PREFIX = 'rk3588.image-preset.'
export function imageModel(data: Record<string, unknown>, index = 0): ImageModel {
  return {
    id: String(data.id ?? `model_${index + 1}`),
    model_type: String(data.model_type ?? 'yolov8_det'),
    model_path: String(data.model_path ?? ''), label_path: String(data.label_path ?? ''),
    obj_thresh: Number(data.obj_thresh ?? .3), nms_thresh: Number(data.nms_thresh ?? .45),
    npu_core: Number(data.npu_core ?? -1),
    detect_classes: Array.isArray(data.detect_classes) ? data.detect_classes.map(String) : [],
  }
}

export function channelImageSettings(selected: Node, nodes: Node[], edges: Edge[]): ImageSettings {
  const incoming = edges.find(e => e.target === selected.id && e.targetHandle === 'stream-in')
  const stream = nodes.find(n => n.id === incoming?.source && n.type === 'stream')
  const linked = new Set(edges.filter(e => e.source === stream?.id && e.sourceHandle === 'stream-out').map(e => e.target))
  const models = stream ? nodes.filter(n => n.type === 'model' && linked.has(n.id) && n.data.enable !== false) : [selected]
  return { models: models.map((n, i) => imageModel(n.data, i)),
    inference_roi: stream?.data.inference_roi as ImageSettings['inference_roi'] }
}

export function openImageInference(app: string, settings: ImageSettings) {
  // A new tab preserves the editor's unsaved canvas. Only a configuration snapshot is shared.
  for (const key of Object.keys(localStorage)) {
    if (key.startsWith(PREFIX)) {
      try {
        if (Date.now() - JSON.parse(localStorage.getItem(key) ?? '{}').created > 86400000) localStorage.removeItem(key)
      } catch { localStorage.removeItem(key) }
    }
  }
  const id = `${Date.now()}-${Math.random().toString(36).slice(2)}`
  localStorage.setItem(PREFIX + id, JSON.stringify({ app, settings, created: Date.now() }))
  window.open(`/image-inference?${new URLSearchParams({ preset: id })}`, '_blank', 'noopener')
}

export function readImagePreset(id: string | null): { app?: string; settings: ImageSettings } | null {
  if (!id) return null
  try {
    const value = JSON.parse(localStorage.getItem(PREFIX + id) ?? 'null')
    if (!value || Date.now() - value.created > 86400000 || !Array.isArray(value.settings?.models)) return null
    return { app: value.app, settings: { models: value.settings.models.map(imageModel), inference_roi: value.settings.inference_roi } }
  } catch { return null }
}
