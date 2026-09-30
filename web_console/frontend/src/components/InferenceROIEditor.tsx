import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { captureSnapshot } from '../api/client'
import { useEditorStore } from '../store/editorStore'
import { getSrcType } from '../utils/streamSource'
import '../nodes/ROINode.css'

export interface NormalizedRect { x: number; y: number; width: number; height: number }
export type NormalizedPoint = [number, number]
export type InferenceRoiMode = 'roi_only' | 'full_plus_roi' | 'full_frame_roi_filter'
export type InferenceRoiShape = 'rect' | 'polygon'
export type InferenceResizeMode = 'stretch' | 'expand' | 'letterbox'
export interface InferenceRoiValue {
  mode: InferenceRoiMode
  shape: InferenceRoiShape
  resizeMode: InferenceResizeMode
  rect: NormalizedRect
  polygon: NormalizedPoint[]
}

const clamp = (value: number, low: number, high: number) => Math.max(low, Math.min(high, value))

export function inferenceRoiConfig(value: unknown): InferenceRoiValue | null {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null
  const config = value as Record<string, unknown>
  if (!['roi_only', 'full_plus_roi', 'full_frame_roi_filter'].includes(String(config.mode))
      || !Array.isArray(config.rect) || config.rect.length !== 4) return null
  const rawRect = config.rect.map(Number)
  if (!rawRect.every(Number.isFinite)) return null
  const [x, y, width, height] = rawRect
  if (x < 0 || y < 0 || width <= 0 || height <= 0
      || x + width > 1 + 1e-9 || y + height > 1 + 1e-9) return null
  const shape: InferenceRoiShape = config.shape === 'polygon' ? 'polygon' : 'rect'
  const resizeMode: InferenceResizeMode = ['stretch', 'expand', 'letterbox'].includes(String(config.resize_mode))
    ? config.resize_mode as InferenceResizeMode : 'stretch'
  const polygon: NormalizedPoint[] = shape === 'polygon' && Array.isArray(config.polygon)
    ? config.polygon.filter((point): point is number[] => Array.isArray(point) && point.length === 2)
      .map(point => [Number(point[0]), Number(point[1])] as NormalizedPoint)
      .filter(point => point.every(Number.isFinite) && point[0] >= 0 && point[0] <= 1 && point[1] >= 0 && point[1] <= 1)
    : []
  if (shape === 'polygon' && (polygon.length < 3 || polygon.length > 64
      || polygonArea(polygon) <= 1e-8 || !polygonIsSimple(polygon))) return null
  return { mode: config.mode as InferenceRoiMode, shape, resizeMode, rect: { x, y, width, height }, polygon }
}

export const inferenceRoiRect = (value: unknown): NormalizedRect | null => inferenceRoiConfig(value)?.rect ?? null
export const inferenceRoiRestrictsResults = (mode: InferenceRoiMode): boolean =>
  mode === 'roi_only' || mode === 'full_frame_roi_filter'

export function inferenceRoiContainsPoint(config: InferenceRoiValue, x: number, y: number): boolean {
  if (config.shape !== 'polygon') {
    const r = config.rect
    return x >= r.x - 1e-9 && x <= r.x + r.width + 1e-9
      && y >= r.y - 1e-9 && y <= r.y + r.height + 1e-9
  }
  return normalizedPointInPolygon(x, y, config.polygon)
}

export function normalizedPointInPolygon(x: number, y: number, polygon: NormalizedPoint[]): boolean {
  let inside = false
  for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
    const [xi, yi] = polygon[i], [xj, yj] = polygon[j]
    const cross = (x - xi) * (yj - yi) - (y - yi) * (xj - xi)
    if (Math.abs(cross) <= 1e-9 && x >= Math.min(xi, xj) - 1e-9 && x <= Math.max(xi, xj) + 1e-9
        && y >= Math.min(yi, yj) - 1e-9 && y <= Math.max(yi, yj) + 1e-9) return true
    if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside
  }
  return inside
}

/** 判断整条业务 ROI 边界都位于推理多边形内；仅检查顶点会漏掉凹口穿越。 */
export function inferenceRoiContainsPolygon(config: InferenceRoiValue, polygon: NormalizedPoint[]): boolean {
  if (polygon.some(([x, y]) => !inferenceRoiContainsPoint(config, x, y))) return false
  if (config.shape !== 'polygon') return true
  const cross = (ax: number, ay: number, bx: number, by: number) => ax * by - ay * bx
  for (let index = 0; index < polygon.length; index++) {
    const a = polygon[index], b = polygon[(index + 1) % polygon.length]
    const rx = b[0] - a[0], ry = b[1] - a[1]
    const parameters = [0, 1]
    for (let edge = 0; edge < config.polygon.length; edge++) {
      const c = config.polygon[edge], d = config.polygon[(edge + 1) % config.polygon.length]
      const sx = d[0] - c[0], sy = d[1] - c[1]
      const denominator = cross(rx, ry, sx, sy)
      const qx = c[0] - a[0], qy = c[1] - a[1]
      if (Math.abs(denominator) > 1e-12) {
        const t = cross(qx, qy, sx, sy) / denominator
        const u = cross(qx, qy, rx, ry) / denominator
        if (t >= -1e-10 && t <= 1 + 1e-10 && u >= -1e-10 && u <= 1 + 1e-10)
          parameters.push(clamp(t, 0, 1))
      } else if (Math.abs(cross(qx, qy, rx, ry)) <= 1e-12) {
        const lengthSquared = rx * rx + ry * ry
        if (lengthSquared > 1e-16) {
          for (const point of [c, d]) {
            const t = ((point[0] - a[0]) * rx + (point[1] - a[1]) * ry) / lengthSquared
            if (t >= -1e-10 && t <= 1 + 1e-10) parameters.push(clamp(t, 0, 1))
          }
        }
      }
    }
    parameters.sort((x, y) => x - y)
    for (let i = 1; i < parameters.length; i++) {
      if (parameters[i] - parameters[i - 1] <= 1e-10) continue
      const t = (parameters[i] + parameters[i - 1]) / 2
      if (!normalizedPointInPolygon(a[0] + rx * t, a[1] + ry * t, config.polygon)) return false
    }
  }
  return true
}

function regionBounds(shape: InferenceRoiShape, rect: NormalizedRect, polygon: NormalizedPoint[]): NormalizedRect {
  if (shape === 'rect' || polygon.length < 3) return rect
  const xs = polygon.map(point => point[0]), ys = polygon.map(point => point[1])
  const x = Math.min(...xs), y = Math.min(...ys)
  return { x, y, width: Math.max(...xs) - x, height: Math.max(...ys) - y }
}

export function polygonArea(polygon: NormalizedPoint[]): number {
  return Math.abs(polygon.reduce((sum, point, index) => {
    const next = polygon[(index + 1) % polygon.length]
    return sum + point[0] * next[1] - next[0] * point[1]
  }, 0)) / 2
}

export function polygonIsSimple(polygon: NormalizedPoint[]): boolean {
  const orientation = (a: NormalizedPoint, b: NormalizedPoint, c: NormalizedPoint) =>
    (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
  const onSegment = (p: NormalizedPoint, a: NormalizedPoint, b: NormalizedPoint) =>
    Math.abs(orientation(a, b, p)) <= 1e-10
      && p[0] >= Math.min(a[0], b[0]) - 1e-10 && p[0] <= Math.max(a[0], b[0]) + 1e-10
      && p[1] >= Math.min(a[1], b[1]) - 1e-10 && p[1] <= Math.max(a[1], b[1]) + 1e-10
  const intersects = (a: NormalizedPoint, b: NormalizedPoint, c: NormalizedPoint, d: NormalizedPoint) => {
    const o1 = orientation(a, b, c), o2 = orientation(a, b, d)
    const o3 = orientation(c, d, a), o4 = orientation(c, d, b)
    if (((o1 > 1e-10 && o2 < -1e-10) || (o1 < -1e-10 && o2 > 1e-10))
        && ((o3 > 1e-10 && o4 < -1e-10) || (o3 < -1e-10 && o4 > 1e-10))) return true
    return onSegment(c, a, b) || onSegment(d, a, b) || onSegment(a, c, d) || onSegment(b, c, d)
  }
  for (let i = 0; i < polygon.length; i++) {
    const next = polygon[(i + 1) % polygon.length]
    if (Math.hypot(polygon[i][0] - next[0], polygon[i][1] - next[1]) <= 1e-10) return false
    for (let j = i + 1; j < polygon.length; j++) {
      if (j === i + 1 || (i === 0 && j === polygon.length - 1)) continue
      if (intersects(polygon[i], next, polygon[j], polygon[(j + 1) % polygon.length])) return false
    }
  }
  return true
}

function usbResolutionForFps(fps: number): { width: number; height: number } {
  if (fps >= 25) return { width: 640, height: 480 }
  if (fps >= 15) return { width: 1280, height: 720 }
  if (fps >= 10) return { width: 1280, height: 960 }
  return { width: 1920, height: 1080 }
}

function serialized(config: InferenceRoiValue): Record<string, unknown> {
  const rect = [config.rect.x, config.rect.y, config.rect.width, config.rect.height].map(value => +value.toFixed(6))
  return {
    mode: config.mode, shape: config.shape, resize_mode: config.resizeMode, rect,
    ...(config.shape === 'polygon'
      ? { polygon: config.polygon.map(point => point.map(value => +value.toFixed(6))) } : {}),
  }
}

export default function InferenceROIEditor({ streamData, onChange }: {
  streamData: Record<string, unknown>
  onChange: (value: Record<string, unknown> | undefined) => void
}) {
  const [show, setShow] = useState(false)
  const [pendingMode, setPendingMode] = useState<InferenceRoiMode>('roi_only')
  const appName = useEditorStore(state => state.appName)
  const config = inferenceRoiConfig(streamData.inference_roi)
  const streamKey = String(streamData.channel_id ?? '')
  const lastConfigsRef = useRef<Map<string, InferenceRoiValue>>(new Map())

  useEffect(() => {
    if (config) lastConfigsRef.current.set(streamKey, config)
  }, [config, streamKey])

  useEffect(() => { setShow(false) }, [streamKey])

  return <>
    <div className="ncp-divider" />
    <div className="ncp-section-label">推理范围</div>
    <select value={config?.mode ?? 'full_frame'} onChange={event => {
      const value = event.target.value
      if (value === 'full_frame') {
        if (config) lastConfigsRef.current.set(streamKey, config)
        onChange(undefined)
        return
      }
      const nextMode = value as InferenceRoiMode
      if (config) onChange(serialized({ ...config, mode: nextMode }))
      else if (lastConfigsRef.current.has(streamKey)) {
        const restored = { ...lastConfigsRef.current.get(streamKey)!, mode: nextMode }
        lastConfigsRef.current.set(streamKey, restored)
        onChange(serialized(restored))
      } else {
        setPendingMode(nextMode)
        setShow(true)
      }
    }}>
      <option value="full_frame">整幅画面</option>
      <option value="full_plus_roi">整幅画面 + 区域增强（每帧两次推理）</option>
      <option value="roi_only">仅推理选定区域</option>
      <option value="full_frame_roi_filter">仅输出区域内目标（保持整帧尺度）</option>
    </select>
    {config ? <>
      <button className="node-btn full primary" onClick={() => { setPendingMode(config.mode); setShow(true) }}>
        编辑推理区域与预览
      </button>
      <div className="ncp-hint">
        {config.shape === 'polygon' ? `单多边形 · ${config.polygon.length} 个顶点` : '矩形'} · {
          config.mode === 'full_frame_roi_filter' ? '整帧尺度'
            : config.resizeMode === 'stretch' ? '直接拉伸'
              : config.resizeMode === 'expand' ? '等比例扩展取景' : '等比例补黑边'}
      </div>
    </> : null}
    {show && createPortal(<InferenceROIDrawModal
      appName={appName} streamData={streamData} initial={config} mode={pendingMode}
      onCancel={() => setShow(false)} onSave={next => {
        lastConfigsRef.current.set(streamKey, next)
        onChange(serialized(next))
        setShow(false)
      }}
    />, document.body)}
  </>
}

type RectDrag =
  | { kind: 'new'; anchor: NormalizedPoint }
  | { kind: 'move'; start: NormalizedPoint; original: NormalizedRect }
  | { kind: 'resize'; corner: number; original: NormalizedRect }
  | { kind: 'vertex'; index: number }
  | { kind: 'polygon-move'; start: NormalizedPoint; original: NormalizedPoint[] }
  | null

interface PreviewTransform { source: NormalizedRect; content: NormalizedRect }

const MODEL_PREVIEW_WIDTH = 640
const MODEL_PREVIEW_HEIGHT = 640

const EXPANSION_INITIAL_HOLD_MS = 800
const EXPANSION_SCALE_MS = 1500
const EXPANSION_SCALE_HOLD_MS = 700
const EXPANSION_MOVE_MS = 2200
const EXPANSION_RESULT_HOLD_MS = 900
const EXPANSION_MOVE_START_MS = EXPANSION_INITIAL_HOLD_MS + EXPANSION_SCALE_MS + EXPANSION_SCALE_HOLD_MS
const EXPANSION_TOTAL_MS = EXPANSION_MOVE_START_MS + EXPANSION_MOVE_MS + EXPANSION_RESULT_HOLD_MS

function fitPreviewContent(source: NormalizedRect, sourceAspect: number): NormalizedRect {
  const cropAspect = source.width * sourceAspect / source.height
  return cropAspect >= 1
    ? { x: 0, y: (1 - 1 / cropAspect) / 2, width: 1, height: 1 / cropAspect }
    : { x: (1 - cropAspect) / 2, y: 0, width: cropAspect, height: 1 }
}

function previewTransform(selection: NormalizedRect, sourceWidth: number, sourceHeight: number,
  mode: InferenceResizeMode): PreviewTransform {
  // 与 C++ 使用相同的像素取整、扩展和 YUV420 偶数边界规则。
  let left = clamp(Math.floor(selection.x * sourceWidth), 0, sourceWidth - 1)
  let top = clamp(Math.floor(selection.y * sourceHeight), 0, sourceHeight - 1)
  let right = clamp(Math.ceil((selection.x + selection.width) * sourceWidth), left + 1, sourceWidth)
  let bottom = clamp(Math.ceil((selection.y + selection.height) * sourceHeight), top + 1, sourceHeight)
  if (mode === 'expand') {
    const targetRatio = MODEL_PREVIEW_WIDTH / MODEL_PREVIEW_HEIGHT
    let width = right - left, height = bottom - top
    if (width / height < targetRatio) width = height * targetRatio
    else height = width / targetRatio
    width = Math.min(width, sourceWidth); height = Math.min(height, sourceHeight)
    const centerX = (left + right) / 2, centerY = (top + bottom) / 2
    left = Math.floor(centerX - width / 2); top = Math.floor(centerY - height / 2)
    right = left + Math.ceil(width); bottom = top + Math.ceil(height)
    if (left < 0) { right -= left; left = 0 }
    if (top < 0) { bottom -= top; top = 0 }
    if (right > sourceWidth) { left -= right - sourceWidth; right = sourceWidth }
    if (bottom > sourceHeight) { top -= bottom - sourceHeight; bottom = sourceHeight }
    left = Math.max(0, left); top = Math.max(0, top)
  }
  left &= ~1; top &= ~1
  right = Math.min(sourceWidth, (right + 1) & ~1)
  bottom = Math.min(sourceHeight, (bottom + 1) & ~1)
  if (right <= left) right = Math.min(sourceWidth, left + 2)
  if (bottom <= top) bottom = Math.min(sourceHeight, top + 2)

  const pixelWidth = right - left, pixelHeight = bottom - top
  let contentX = 0, contentY = 0, contentWidth = MODEL_PREVIEW_WIDTH, contentHeight = MODEL_PREVIEW_HEIGHT
  if (mode !== 'stretch') {
    const scale = Math.min(MODEL_PREVIEW_WIDTH / pixelWidth, MODEL_PREVIEW_HEIGHT / pixelHeight)
    contentWidth = clamp(Math.round(pixelWidth * scale), 1, MODEL_PREVIEW_WIDTH)
    contentHeight = clamp(Math.round(pixelHeight * scale), 1, MODEL_PREVIEW_HEIGHT)
    contentX = Math.floor((MODEL_PREVIEW_WIDTH - contentWidth) / 2)
    contentY = Math.floor((MODEL_PREVIEW_HEIGHT - contentHeight) / 2)
  }
  return {
    source: { x: left / sourceWidth, y: top / sourceHeight,
      width: pixelWidth / sourceWidth, height: pixelHeight / sourceHeight },
    content: { x: contentX / MODEL_PREVIEW_WIDTH, y: contentY / MODEL_PREVIEW_HEIGHT,
      width: contentWidth / MODEL_PREVIEW_WIDTH, height: contentHeight / MODEL_PREVIEW_HEIGHT },
  }
}

function InferenceROIDrawModal({ appName, streamData, initial, mode, onCancel, onSave }: {
  appName: string
  streamData: Record<string, unknown>
  initial: InferenceRoiValue | null
  mode: InferenceRoiMode
  onCancel: () => void
  onSave: (value: InferenceRoiValue) => void
}) {
  const globalMaxFps = useEditorStore(state => state.globalMaxFps)
  const canvasRef = useRef<HTMLCanvasElement>(null), previewRef = useRef<HTMLCanvasElement>(null)
  const [srcW, setSrcW] = useState(1920), [srcH, setSrcH] = useState(1080)
  const [bgImage, setBgImage] = useState<HTMLImageElement | null>(null)
  const [loading, setLoading] = useState(false), [error, setError] = useState('')
  const [shape, setShape] = useState<InferenceRoiShape>(initial?.shape ?? 'rect')
  const [resizeMode, setResizeMode] = useState<InferenceResizeMode>(initial?.resizeMode ?? 'stretch')
  const [rect, setRect] = useState<NormalizedRect>(initial?.rect ?? { x: .25, y: .25, width: .5, height: .5 })
  const [polygon, setPolygon] = useState<NormalizedPoint[]>(initial?.polygon ?? [])
  const [drawingPolygon, setDrawingPolygon] = useState(false)
  const [drag, setDrag] = useState<RectDrag>(null)
  const [previewPass, setPreviewPass] = useState<'full' | 'roi'>('roi')
  const [expansionElapsed, setExpansionElapsed] = useState(EXPANSION_TOTAL_MS)
  const expansionFrameRef = useRef<number | null>(null)
  const dispW = Math.max(480, Math.min(760, window.innerWidth - 520))
  const dispH = Math.round(dispW * srcH / srcW)
  const previewSize = Math.max(240, Math.min(360, dispH))
  // 配置保存为 6 位小数；预览也先做相同量化，保证保存前后不跳动。
  const runtimePolygon = useMemo<NormalizedPoint[]>(() => polygon.map(([x, y]) =>
    [+x.toFixed(6), +y.toFixed(6)]), [polygon])
  const selection = useMemo(() => shape === 'polygon'
    ? regionBounds(shape, rect, runtimePolygon)
    : { x: +rect.x.toFixed(6), y: +rect.y.toFixed(6),
      width: +rect.width.toFixed(6), height: +rect.height.toFixed(6) }, [shape, rect, runtimePolygon])
  const transform = useMemo(() => mode === 'full_frame_roi_filter'
    ? { source: { x: 0, y: 0, width: 1, height: 1 }, content: { x: 0, y: 0, width: 1, height: 1 } }
    : previewTransform(selection, srcW, srcH, resizeMode), [mode, selection, srcW, srcH, resizeMode])
  const showFullPass = mode === 'full_plus_roi' && previewPass === 'full'
  const playExpansion = useCallback(() => {
    if (expansionFrameRef.current != null) cancelAnimationFrame(expansionFrameRef.current)
    if (window.matchMedia?.('(prefers-reduced-motion: reduce)').matches) {
      setExpansionElapsed(EXPANSION_TOTAL_MS)
      return
    }
    const startedAt = performance.now()
    setExpansionElapsed(0)
    const step = (now: number) => {
      const elapsed = Math.min(EXPANSION_TOTAL_MS, now - startedAt)
      setExpansionElapsed(elapsed)
      expansionFrameRef.current = elapsed < EXPANSION_TOTAL_MS ? requestAnimationFrame(step) : null
    }
    expansionFrameRef.current = requestAnimationFrame(step)
  }, [])
  const animatedTransform = useMemo<PreviewTransform>(() => {
    if (resizeMode !== 'expand' || mode === 'full_frame_roi_filter') return transform
    const scaleLinear = clamp((expansionElapsed - EXPANSION_INITIAL_HOLD_MS) / EXPANSION_SCALE_MS, 0, 1)
    const moveLinear = clamp((expansionElapsed - EXPANSION_MOVE_START_MS) / EXPANSION_MOVE_MS, 0, 1)
    const scaleProgress = scaleLinear * scaleLinear * (3 - 2 * scaleLinear)
    const moveProgress = moveLinear * moveLinear * (3 - 2 * moveLinear)
    const source: NormalizedRect = {
      x: selection.x + (transform.source.x - selection.x) * moveProgress,
      y: selection.y + (transform.source.y - selection.y) * moveProgress,
      width: selection.width + (transform.source.width - selection.width) * moveProgress,
      height: selection.height + (transform.source.height - selection.height) * moveProgress,
    }
    const fitted = fitPreviewContent(source, srcW / srcH)
    const contentScale = 0.55 + 0.45 * scaleProgress
    return { source, content: {
      x: 0.5 - fitted.width * contentScale / 2,
      y: 0.5 - fitted.height * contentScale / 2,
      width: fitted.width * contentScale,
      height: fitted.height * contentScale,
    } }
  }, [expansionElapsed, mode, resizeMode, selection, srcW, srcH, transform])

  const expansionStatus = expansionElapsed < EXPANSION_INITIAL_HOLD_MS
    ? '第 1 步：原始选区（停留）'
    : expansionElapsed < EXPANSION_INITIAL_HOLD_MS + EXPANSION_SCALE_MS
      ? `第 2 步：适配模型画布 ${Math.round(
        (expansionElapsed - EXPANSION_INITIAL_HOLD_MS) / EXPANSION_SCALE_MS * 100)}%`
      : expansionElapsed < EXPANSION_MOVE_START_MS
        ? '尺寸适配完成（停留观察）'
        : expansionElapsed < EXPANSION_MOVE_START_MS + EXPANSION_MOVE_MS
          ? `第 3 步：扩展周边 ${Math.round(
            (expansionElapsed - EXPANSION_MOVE_START_MS) / EXPANSION_MOVE_MS * 100)}%`
          : expansionElapsed < EXPANSION_TOTAL_MS
            ? '最终视野（停留观察）'
          : '640×640 比例'

  useEffect(() => {
    if (resizeMode === 'expand' && mode !== 'full_frame_roi_filter') playExpansion()
    else setExpansionElapsed(EXPANSION_TOTAL_MS)
    return () => {
      if (expansionFrameRef.current != null) cancelAnimationFrame(expansionFrameRef.current)
    }
  }, [bgImage, mode, playExpansion, resizeMode])

  const grabFrame = async () => {
    if (!appName) { setError('未选择程序'); return }
    setLoading(true); setError('')
    try {
      const srcType = getSrcType(streamData)
      const explicitW = Number(streamData.usb_width ?? 0), explicitH = Number(streamData.usb_height ?? 0)
      const maxFps = Number(streamData.max_fps ?? 0) || globalMaxFps || 15
      const usbRes = explicitW > 0 && explicitH > 0 ? { width: explicitW, height: explicitH } : usbResolutionForFps(maxFps)
      const result = await captureSnapshot(appName, {
        src_type: srcType, url: String(streamData.url ?? ''), device: String(streamData.device ?? '/dev/video81'),
        ...(srcType === 'usb' ? { usb_width: usbRes.width, usb_height: usbRes.height } : {}),
      })
      setSrcW(result.width); setSrcH(result.height)
      const image = new Image(); image.onload = () => setBgImage(image); image.src = result.image
    } catch (cause: unknown) {
      const message = cause instanceof Error ? cause.message : String(cause)
      setError(getSrcType(streamData) === 'usb' ? `USB 摄像头可能正被程序占用。${message}` : message)
    } finally { setLoading(false) }
  }

  const pathRegion = useCallback((ctx: CanvasRenderingContext2D, width: number, height: number) => {
    ctx.beginPath()
    if (shape === 'rect') ctx.rect(rect.x * width, rect.y * height, rect.width * width, rect.height * height)
    else polygon.forEach((point, index) => index === 0
      ? ctx.moveTo(point[0] * width, point[1] * height) : ctx.lineTo(point[0] * width, point[1] * height))
    if (shape === 'polygon' && polygon.length >= 3) ctx.closePath()
  }, [shape, rect, polygon])

  const render = useCallback(() => {
    const canvas = canvasRef.current, preview = previewRef.current
    if (!canvas || !preview) return
    const ctx = canvas.getContext('2d')!, pctx = preview.getContext('2d')!
    ctx.clearRect(0, 0, dispW, dispH)
    if (bgImage) ctx.drawImage(bgImage, 0, 0, dispW, dispH)
    else { ctx.fillStyle = '#1a1f2e'; ctx.fillRect(0, 0, dispW, dispH); ctx.fillStyle = '#64748b'; ctx.textAlign = 'center'; ctx.fillText(loading ? '正在抓取当前帧…' : '暂无画面', dispW / 2, dispH / 2) }
    if ((shape === 'rect' || polygon.length >= 3) && !drawingPolygon) {
      ctx.save(); ctx.fillStyle = 'rgba(2,6,23,.64)'; ctx.fillRect(0, 0, dispW, dispH)
      ctx.globalCompositeOperation = 'destination-out'; pathRegion(ctx, dispW, dispH); ctx.fill(); ctx.restore()
    }
    pathRegion(ctx, dispW, dispH); ctx.strokeStyle = '#22d3ee'; ctx.lineWidth = 3; ctx.stroke()
    const points = shape === 'rect'
      ? [[rect.x, rect.y], [rect.x + rect.width, rect.y], [rect.x + rect.width, rect.y + rect.height], [rect.x, rect.y + rect.height]]
      : polygon
    points.forEach(([x, y], index) => { ctx.fillStyle = '#ecfeff'; ctx.fillRect(x * dispW - 5, y * dispH - 5, 10, 10); if (shape === 'polygon') { ctx.fillStyle = '#083344'; ctx.font = '10px sans-serif'; ctx.textAlign = 'center'; ctx.fillText(String(index + 1), x * dispW, y * dispH + 3) } })

    pctx.fillStyle = '#000'; pctx.fillRect(0, 0, previewSize, previewSize)
    const source = showFullPass ? { x: 0, y: 0, width: 1, height: 1 } : animatedTransform.source
    const content = showFullPass ? { x: 0, y: 0, width: 1, height: 1 } : animatedTransform.content
    const previewPoints: NormalizedPoint[] = shape === 'polygon' ? runtimePolygon : [
      [rect.x, rect.y], [rect.x + rect.width, rect.y],
      [rect.x + rect.width, rect.y + rect.height], [rect.x, rect.y + rect.height],
    ]
    const pathPreviewRegion = () => {
      pctx.beginPath()
      previewPoints.forEach((point, index) => {
        const x = Math.round((content.x + (point[0] - source.x) / source.width * content.width)
          * MODEL_PREVIEW_WIDTH) / MODEL_PREVIEW_WIDTH
        const y = Math.round((content.y + (point[1] - source.y) / source.height * content.height)
          * MODEL_PREVIEW_HEIGHT) / MODEL_PREVIEW_HEIGHT
        if (index === 0) pctx.moveTo(x * previewSize, y * previewSize); else pctx.lineTo(x * previewSize, y * previewSize)
      })
      pctx.closePath()
    }
    const strictPolygonMask = shape === 'polygon' && runtimePolygon.length >= 3
      && resizeMode !== 'expand' && mode !== 'full_frame_roi_filter' && !showFullPass
    pctx.save()
    if (strictPolygonMask) { pathPreviewRegion(); pctx.clip() }
    if (bgImage) pctx.drawImage(bgImage, source.x * bgImage.width, source.y * bgImage.height,
      source.width * bgImage.width, source.height * bgImage.height,
      content.x * previewSize, content.y * previewSize, content.width * previewSize, content.height * previewSize)
    else { pctx.fillStyle = '#1a1f2e'; pctx.fillRect(content.x * previewSize, content.y * previewSize, content.width * previewSize, content.height * previewSize) }
    pctx.restore()
    // 青色轮廓只属于 expand 的教学动画；最终帧不叠加任何模型未收到的图形。
    if (resizeMode === 'expand' && mode !== 'full_frame_roi_filter' && !showFullPass
        && expansionElapsed < EXPANSION_TOTAL_MS) {
      pathPreviewRegion(); pctx.strokeStyle = '#22d3ee'; pctx.lineWidth = 2
      pctx.setLineDash([7, 5]); pctx.stroke(); pctx.setLineDash([])
    }
  }, [animatedTransform, bgImage, loading, shape, rect, polygon, runtimePolygon, drawingPolygon, dispW, dispH,
    previewSize, pathRegion, resizeMode, mode, showFullPass, expansionElapsed])

  useEffect(() => { render() }, [render])
  // eslint-disable-next-line react-hooks/exhaustive-deps
  useEffect(() => { grabFrame() }, [])

  const position = (event: React.MouseEvent<HTMLCanvasElement>): NormalizedPoint => {
    const bounds = canvasRef.current!.getBoundingClientRect()
    return [clamp((event.clientX - bounds.left) / bounds.width, 0, 1), clamp((event.clientY - bounds.top) / bounds.height, 0, 1)]
  }
  const nearestPoint = (point: NormalizedPoint, points: NormalizedPoint[]) => points.findIndex(candidate =>
    Math.hypot((point[0] - candidate[0]) * dispW, (point[1] - candidate[1]) * dispH) <= 12)
  const mouseDown = (event: React.MouseEvent<HTMLCanvasElement>) => {
    const point = position(event)
    if (shape === 'polygon') {
      if (drawingPolygon) {
        if (polygon.length >= 3 && nearestPoint(point, [polygon[0]]) === 0) { setDrawingPolygon(false); return }
        if (polygon.length < 64) setPolygon([...polygon, point])
        return
      }
      const vertex = nearestPoint(point, polygon)
      if (vertex >= 0) { setDrag({ kind: 'vertex', index: vertex }); return }
      if (normalizedPointInPolygon(point[0], point[1], polygon))
        setDrag({ kind: 'polygon-move', start: point, original: polygon })
      return
    }
    const corners: NormalizedPoint[] = [[rect.x, rect.y], [rect.x + rect.width, rect.y], [rect.x + rect.width, rect.y + rect.height], [rect.x, rect.y + rect.height]]
    const corner = nearestPoint(point, corners)
    if (corner >= 0) setDrag({ kind: 'resize', corner, original: rect })
    else if (point[0] >= rect.x && point[0] <= rect.x + rect.width && point[1] >= rect.y && point[1] <= rect.y + rect.height)
      setDrag({ kind: 'move', start: point, original: rect })
    else { setRect({ x: point[0], y: point[1], width: 16 / dispW, height: 16 / dispH }); setDrag({ kind: 'new', anchor: point }) }
  }
  const mouseMove = (event: React.MouseEvent<HTMLCanvasElement>) => {
    if (!drag) return
    const point = position(event), minX = 16 / dispW, minY = 16 / dispH
    if (drag.kind === 'vertex') { setPolygon(old => old.map((value, index) => index === drag.index ? point : value)); return }
    if (drag.kind === 'polygon-move') {
      const xs = drag.original.map(value => value[0]), ys = drag.original.map(value => value[1])
      const dx = clamp(point[0] - drag.start[0], -Math.min(...xs), 1 - Math.max(...xs))
      const dy = clamp(point[1] - drag.start[1], -Math.min(...ys), 1 - Math.max(...ys))
      setPolygon(drag.original.map(([x, y]) => [x + dx, y + dy]))
      return
    }
    if (drag.kind === 'move') { setRect({ ...drag.original,
      x: clamp(drag.original.x + point[0] - drag.start[0], 0, 1 - drag.original.width),
      y: clamp(drag.original.y + point[1] - drag.start[1], 0, 1 - drag.original.height) }); return }
    if (drag.kind === 'new') { const x = Math.min(drag.anchor[0], point[0]), y = Math.min(drag.anchor[1], point[1]); setRect({ x, y,
      width: Math.min(1 - x, Math.max(minX, Math.abs(point[0] - drag.anchor[0]))),
      height: Math.min(1 - y, Math.max(minY, Math.abs(point[1] - drag.anchor[1]))) }); return }
    const right = drag.original.x + drag.original.width, bottom = drag.original.y + drag.original.height
    const left = drag.corner === 0 || drag.corner === 3 ? Math.min(point[0], right - minX) : drag.original.x
    const top = drag.corner === 0 || drag.corner === 1 ? Math.min(point[1], bottom - minY) : drag.original.y
    const nextRight = drag.corner === 1 || drag.corner === 2 ? Math.max(point[0], left + minX) : right
    const nextBottom = drag.corner === 2 || drag.corner === 3 ? Math.max(point[1], top + minY) : bottom
    setRect({ x: clamp(left, 0, 1), y: clamp(top, 0, 1), width: clamp(nextRight, 0, 1) - clamp(left, 0, 1), height: clamp(nextBottom, 0, 1) - clamp(top, 0, 1) })
  }
  const switchShape = (next: InferenceRoiShape) => {
    if (next === 'polygon' && polygon.length < 3) setPolygon([[rect.x, rect.y], [rect.x + rect.width, rect.y], [rect.x + rect.width, rect.y + rect.height], [rect.x, rect.y + rect.height]])
    if (next === 'rect' && polygon.length >= 3) setRect(regionBounds('polygon', rect, polygon))
    setShape(next); setDrawingPolygon(false)
  }
  const validRect = rect.width > 1e-8 && rect.height > 1e-8
  const validPolygon = polygon.length >= 3 && !drawingPolygon
    && polygonArea(polygon) > 1e-8 && polygonIsSimple(polygon)
  const valid = shape === 'rect' ? validRect : validPolygon
  const inferenceCursor = drawingPolygon ? 'crosshair'
    : drag ? (drag.kind === 'vertex' ? 'grabbing' : 'move')
      : 'default'
  const handleContextMenu = (event: React.MouseEvent<HTMLCanvasElement>) => {
    event.preventDefault()
    if (shape !== 'polygon') return
    if (drawingPolygon) { setPolygon(old => old.slice(0, -1)); return }
    const vertex = nearestPoint(position(event), polygon)
    if (vertex >= 0 && polygon.length > 3) setPolygon(old => old.filter((_, index) => index !== vertex))
  }

  return <div className="roi-overlay"><div className="roi-dialog inference-roi-dialog">
    <div className="roi-hdr"><span>单区域推理设置</span><div className="roi-hdr-actions">
      <button className="roi-grab-btn" onClick={grabFrame} disabled={loading}>{loading ? '抓取中…' : '📷 抓取当前帧'}</button>
      {error && <span className="roi-err">{error}</span>}<span className="roi-res">{srcW}×{srcH}</span>
      <button className="roi-close-btn" onClick={onCancel}>✕</button>
    </div></div>
    <div className="inference-roi-toolbar">
      <label>区域形状 <select value={shape} onChange={event => switchShape(event.target.value as InferenceRoiShape)}><option value="rect">矩形</option><option value="polygon">单个多边形</option></select></label>
      {mode !== 'full_frame_roi_filter' && <label>模型取景 <select value={resizeMode} onChange={event => setResizeMode(event.target.value as InferenceResizeMode)}><option value="stretch">直接拉伸（可能变形）</option><option value="expand">等比例扩展取景</option><option value="letterbox">等比例缩放 + 黑边</option></select></label>}
      {shape === 'polygon' && <><button className="roi-grab-btn" onClick={() => { setPolygon([]); setDrawingPolygon(true) }}>重新绘制</button>{drawingPolygon && <button className="roi-grab-btn" disabled={polygon.length < 3} onClick={() => setDrawingPolygon(false)}>闭合多边形</button>}</>}
    </div>
    <div className="inference-roi-help">{drawingPolygon
      ? `点击添加顶点（${polygon.length} 个），点击首点或按钮闭合。`
      : shape === 'polygon' && !validPolygon ? '多边形存在重合边、自相交或面积为零，请重新绘制或调整顶点。'
      : shape === 'polygon' ? (mode === 'full_plus_roi'
        ? '拖顶点调整，拖内部移动；整帧批次仍保留完整结果。'
        : '拖顶点调整，拖内部移动，右键删除顶点。')
        : '拖空白处重画，拖内部移动，拖四角缩放。'}</div>
    <div className="inference-roi-workspace"><div className="roi-canvas-wrap"><canvas ref={canvasRef} width={dispW} height={dispH} className="roi-canvas"
      style={{ width: dispW, height: dispH, cursor: inferenceCursor }}
      onMouseDown={mouseDown} onMouseMove={mouseMove} onMouseUp={() => setDrag(null)} onMouseLeave={() => setDrag(null)}
      onContextMenu={handleContextMenu} /></div>
      <div className="inference-preview-panel"><div className="inference-preview-title"><span>模型实际视野</span><span className="inference-preview-status">
        {mode === 'full_plus_roi' && <>
          <button type="button" className={previewPass === 'full' ? 'active' : ''}
            onClick={() => setPreviewPass('full')}>整帧批次</button>
          <button type="button" className={previewPass === 'roi' ? 'active' : ''}
            onClick={() => setPreviewPass('roi')}>局部批次</button>
        </>}
        {resizeMode === 'expand' && mode !== 'full_frame_roi_filter' && !showFullPass
          && <button type="button" onClick={playExpansion}>▶ 重播</button>}
        <span>{resizeMode === 'expand' && !showFullPass ? expansionStatus : '640×640 比例'}</span>
      </span></div>
        <canvas ref={previewRef} width={previewSize} height={previewSize} style={{ width: previewSize, height: previewSize }} />
        <div className="inference-preview-note">{showFullPass ? '整帧直接缩放到模型输入。'
          : mode === 'full_frame_roi_filter' ? '模型看整帧；青色区域仅决定哪些结果可以输出。'
          : resizeMode === 'stretch' ? (shape === 'polygon'
            ? '包围框拉伸；多边形外填黑。' : '选区填满输入，可能发生形变。')
            : resizeMode === 'expand' ? '动画结束后的画面是实际模型输入。'
              : shape === 'polygon' ? '保持比例；多边形外和剩余空间填黑。'
                : '选区保持比例完整显示，剩余空间填黑。'}</div></div></div>
    <div className="roi-footer"><span /><div className="roi-actions">
      <button className="roi-btn" onClick={onCancel}>取消</button><button className="roi-btn primary" disabled={!valid} onClick={() => {
        const bounds = regionBounds(shape, rect, polygon)
        onSave({ mode, shape, resizeMode, rect: bounds, polygon: shape === 'polygon' ? polygon : [] })
      }}>保存单区域设置</button></div></div>
  </div></div>
}
