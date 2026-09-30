import { Handle, Position, NodeProps, useReactFlow } from '@xyflow/react'
import { useState, useRef, useEffect, useCallback } from 'react'
import { createPortal } from 'react-dom'
import { useROIStore, type Zone } from '../store/roiStore'
import { useEditorStore } from '../store/editorStore'
import { captureSnapshot } from '../api/client'
import { getSrcType } from '../utils/streamSource'
import { normalizeRoiPolygon } from '../utils/roiPolygon'
import {
  inferenceRoiConfig,
  inferenceRoiContainsPoint,
  inferenceRoiContainsPolygon,
  inferenceRoiRestrictsResults,
  polygonArea,
  polygonIsSimple,
  type InferenceRoiValue,
} from '../components/InferenceROIEditor'
import './nodeStyles.css'
import './ROINode.css'

const EMPTY_ZONES: Zone[] = []
const CANVAS_MAX_W = 880
const SNAP_PX = 10

// 每个区域一种颜色, 区域多于色板时循环复用 (与 C++ render/逻辑里的配色意图一致)
const ZONE_COLORS = ['#fbbf24', '#34d399', '#60a5fa', '#f472b6', '#fb923c', '#a78bfa']

/**
 * 根据 max_fps 推算 GStreamer USB 管道实际采集分辨率。
 * 必须与 createUsbDecChannel() 中的档位逻辑完全一致，否则 ROI 坐标系会错位。
 */
function usbResolutionForFps(fps: number): { width: number; height: number } {
  if (fps >= 25) return { width: 640,  height: 480  }
  if (fps >= 15) return { width: 1280, height: 720  }
  if (fps >= 10) return { width: 1280, height: 960  }
  return             { width: 1920, height: 1080 }
}

// 加载新旧配置时统一去掉末尾重复的闭合点，编辑期只保留实际顶点。
function stripClose(poly: number[][]): [number, number][] {
  return normalizeRoiPolygon(poly).map(p => [p[0], p[1]] as [number, number])
}

const clamp01 = (v: number) => (v < 0 ? 0 : v > 1 ? 1 : v)

// 射线法: 判断归一化点(x,y)是否落在归一化多边形 pts 内
function pointInPoly(x: number, y: number, pts: [number, number][]): boolean {
  let inside = false
  for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) {
    const xi = pts[i][0], yi = pts[i][1], xj = pts[j][0], yj = pts[j][1]
    if (((yi > y) !== (yj > y)) && (x < ((xj - xi) * (y - yi)) / (yj - yi) + xi)) inside = !inside
  }
  return inside
}

export default function ROINode({ id, selected }: NodeProps) {
  const zones        = useROIStore(s => s.zones[id] ?? EMPTY_ZONES)
  const clearZones   = useROIStore(s => s.clearZones)
  const appName      = useEditorStore(s => s.appName)
  const globalMaxFps = useEditorStore(s => s.globalMaxFps)
  const rf           = useReactFlow()

  const [showModal, setShowModal] = useState(false)
  const nZones = zones.length

  // ── ROI 直接归属于视频流；从连接的视频流读取抓帧和 USB 分辨率信息。 ──
  const getStreamInfo = (): {
    streamData: Record<string, unknown> | null
    usbRes: { width: number; height: number } | null
    inferenceRegion: InferenceRoiValue | null
  } => {
    const edges = rf.getEdges()
    const toStream = edges.find(e =>
      e.source === id && e.sourceHandle === 'roi-out' &&
      e.targetHandle === 'roi-in' && rf.getNode(e.target)?.type === 'stream')
    if (!toStream) return { streamData: null, usbRes: null, inferenceRegion: null }
    const streamData = (rf.getNode(toStream.target)?.data as Record<string, unknown>) ?? null

    let usbRes: { width: number; height: number } | null = null
    if (getSrcType(streamData) === 'usb') {
      const ew = Number(streamData.usb_width ?? 0)
      const eh = Number(streamData.usb_height ?? 0)
      if (ew > 0 && eh > 0) {
        usbRes = { width: ew, height: eh }
      } else {
        // playback_fps 仍来自模型节点兼容数据；通道最大 FPS 则存放在视频流节点。
        const streamOutput = edges.find(e =>
          e.source === toStream.target && e.sourceHandle === 'stream-out' &&
          rf.getNode(e.target)?.type === 'model')
        const anchorData = streamOutput
          ? rf.getNode(streamOutput.target)?.data as Record<string, unknown> | undefined
          : undefined
        const playbackFps = Number(anchorData?.playback_fps ?? 0)
        const maxFps      = Number(streamData?.max_fps      ?? 0)
        const fps = playbackFps > 0 ? playbackFps
                  : maxFps      > 0 ? maxFps
                  : globalMaxFps > 0 ? globalMaxFps
                  : 15
        usbRes = usbResolutionForFps(fps)
      }
    }
    const inferenceConfig = inferenceRoiConfig(streamData?.inference_roi)
    return {
      streamData,
      usbRes,
      inferenceRegion: inferenceConfig && inferenceRoiRestrictsResults(inferenceConfig.mode)
        ? inferenceConfig : null,
    }
  }

  return (
    <>
      <div className={`rf-node${selected ? ' selected' : ''}`} style={{ minWidth: 210 }}>
        <div className="rf-node-header header-roi">
          <span>⬡</span><span>业务 ROI</span>
          {nZones > 0 && <span className="roi-count-badge">{nZones}</span>}
        </div>

        <div className="rf-node-body">
          <div className={`roi-chip${nZones > 0 ? ' active' : ''}`}>
            {nZones > 0 ? `✔ 已配置 ${nZones} 个区域` : '🔲 尚未配置区域'}
          </div>
          {nZones > 0 && (
            <div className="roi-zone-list">
              {zones.map((z, i) => (
                <div key={i} className="roi-zone-line">
                  <span className="roi-zone-dot" style={{ background: ZONE_COLORS[i % ZONE_COLORS.length] }} />
                  <span className="roi-zone-name">{z.name?.trim() || `区域${i + 1}`}</span>
                </div>
              ))}
            </div>
          )}
          <button className="node-btn full primary" onClick={() => setShowModal(true)}>
            {nZones > 0 ? '编辑业务 ROI' : '绘制业务 ROI'}
          </button>
          {nZones > 0 && (
            <button className="node-btn full danger" onClick={() => clearZones(id)}>
              清除全部区域
            </button>
          )}
        </div>

        <Handle type="source" position={Position.Bottom} id="roi-out" />
      </div>

      {showModal && createPortal(
        <ROIDrawModal
          nodeId={id}
          appName={appName}
          {...getStreamInfo()}
          onClose={() => setShowModal(false)}
        />,
        document.body
      )}
    </>
  )
}

// ───────────────────────────────────────────────────────────────────────────
// ROI Draw Modal — 单张画面上画多个命名区域
// ───────────────────────────────────────────────────────────────────────────
interface ModalProps {
  nodeId: string
  appName: string
  streamData: Record<string, unknown> | null
  usbRes: { width: number; height: number } | null
  inferenceRegion: InferenceRoiValue | null
  onClose: () => void
}

// 编辑期内部表示: 顶点为归一化(0~1)、不含闭合重复点
interface WZone { name: string; pts: [number, number][] }

function nextDefaultZoneName(zones: WZone[]): string {
  const usedNames = new Set(zones.map(z => z.name.trim()).filter(Boolean))
  let index = 1
  while (usedNames.has(`区域${index}`)) index++
  return `区域${index}`
}

// 编辑(非绘制)态的拖拽: 拖某顶点, 或整块移动某区域
type DragState =
  | { kind: 'vertex'; zi: number; vi: number }
  | { kind: 'zone'; zi: number; lastX: number; lastY: number }
  | null

function ROIDrawModal({ nodeId, appName, streamData, usbRes, inferenceRegion, onClose }: ModalProps) {
  const storeZones = useROIStore(s => s.zones[nodeId] ?? EMPTY_ZONES)
  const setZones   = useROIStore(s => s.setZones)
  const clearZones = useROIStore(s => s.clearZones)
  const storedRes  = useROIStore(s => s.resolutions[nodeId])

  const canvasRef = useRef<HTMLCanvasElement>(null)

  const [srcW,    setSrcW   ] = useState(() => storedRes?.[0] ?? 1920)
  const [srcH,    setSrcH   ] = useState(() => storedRes?.[1] ?? 1080)
  const [bgImage, setBgImage] = useState<HTMLImageElement | null>(null)
  const [loading, setLoading] = useState(false)
  const [snapErr, setSnapErr] = useState('')

  // 已完成的区域(归一化顶点) + 进行中的草稿(显示像素) + 选中高亮项
  const [wzones,  setWzones ] = useState<WZone[]>(() =>
    storeZones.map(z => ({ name: z.name ?? '', pts: stripClose(z.polygon) })))
  const [draft,   setDraft  ] = useState<[number, number][]>([])
  const [drawing, setDrawing] = useState(false)
  const [hover,   setHover  ] = useState<[number, number] | null>(null)
  const [sel,     setSel    ] = useState<number>(-1)
  const [drag,    setDrag   ] = useState<DragState>(null)              // 正在拖拽的顶点/区域
  const [hoverHit, setHoverHit] = useState<'vertex' | 'zone' | null>(null) // 悬停命中(驱动光标)

  // ROI 名称在保存时会 trim，因此重复检查也按去除首尾空格后的名称进行。
  const duplicateNames = (() => {
    const seen = new Set<string>()
    const duplicates = new Set<string>()
    wzones.forEach(z => {
      const name = z.name.trim()
      if (!name) return
      if (seen.has(name)) duplicates.add(name)
      else seen.add(name)
    })
    return duplicates
  })()
  const pointAllowed = (x: number, y: number) =>
    !inferenceRegion || inferenceRoiContainsPoint(inferenceRegion, x, y)
  const zoneAllowed = (points: [number, number][]) =>
    points.length >= 3 && points.length <= 64 && polygonArea(points) > 1e-8 && polygonIsSimple(points)
      && (!inferenceRegion || inferenceRoiContainsPolygon(inferenceRegion, points))
  const invalidZoneIndexes = new Set(wzones.flatMap((zone, index) =>
    !zoneAllowed(zone.pts) ? [index] : []))

  // 画布显示尺寸(给右侧区域列表留出空间, 保持宽高比)
  const dispW = Math.max(360, Math.min(CANVAS_MAX_W, window.innerWidth - 380))
  const dispH = Math.round(dispW * srcH / srcW)

  // ── Grab frame from backend ──
  const grabFrame = async () => {
    if (!streamData || !appName) { setSnapErr('请先连接视频流节点'); return }
    setLoading(true); setSnapErr('')
    try {
      const src_type = getSrcType(streamData)
      const res = await captureSnapshot(appName, {
        src_type,
        url:    String(streamData.url    ?? ''),
        device: String(streamData.device ?? '/dev/video81'),
        ...(src_type === 'usb' && usbRes
          ? { usb_width: usbRes.width, usb_height: usbRes.height }
          : {}),
      })
      setSrcW(res.width)
      setSrcH(res.height)
      const img = new Image()
      img.onload = () => setBgImage(img)
      img.src    = res.image
      // 归一化区域与分辨率无关, 无需重算; 仅丢弃进行中的草稿避免错位
      setDraft([]); setDrawing(false); setHover(null)
    } catch (e: unknown) {
      const raw = e instanceof Error ? e.message : String(e)
      if (getSrcType(streamData) === 'usb') {
        setSnapErr(
          'USB 摄像头打开失败：该设备同一时刻只能被一个进程占用，通常是「正在运行的程序」占着它。' +
          '请先到「程序管理」停止该程序，再抓取 ROI（ROI 改完本来也要停止→启动才生效，顺路即可）。' +
          `原始错误：${raw}`
        )
      } else {
        setSnapErr(raw)
      }
    } finally {
      setLoading(false)
    }
  }

  // ── Canvas render ──
  const render = useCallback(() => {
    const canvas = canvasRef.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')!
    ctx.clearRect(0, 0, dispW, dispH)

    if (bgImage) ctx.drawImage(bgImage, 0, 0, dispW, dispH)
    else {
      ctx.fillStyle = '#1a1f2e'; ctx.fillRect(0, 0, dispW, dispH)
      ctx.fillStyle = '#334155'; ctx.font = '14px sans-serif'; ctx.textAlign = 'center'
      ctx.fillText('正在抓取当前帧…', dispW / 2, dispH / 2)
    }
    if (!bgImage) return

    if (inferenceRegion) {
      const regionPoints = inferenceRegion.shape === 'polygon' ? inferenceRegion.polygon : [
        [inferenceRegion.rect.x, inferenceRegion.rect.y],
        [inferenceRegion.rect.x + inferenceRegion.rect.width, inferenceRegion.rect.y],
        [inferenceRegion.rect.x + inferenceRegion.rect.width, inferenceRegion.rect.y + inferenceRegion.rect.height],
        [inferenceRegion.rect.x, inferenceRegion.rect.y + inferenceRegion.rect.height],
      ]
      ctx.save(); ctx.fillStyle = 'rgba(2, 6, 23, 0.62)'; ctx.fillRect(0, 0, dispW, dispH)
      ctx.beginPath(); regionPoints.forEach(([x, y], index) => index === 0
        ? ctx.moveTo(x * dispW, y * dispH) : ctx.lineTo(x * dispW, y * dispH)); ctx.closePath()
      ctx.globalCompositeOperation = 'destination-out'; ctx.fill(); ctx.restore()
      ctx.beginPath(); regionPoints.forEach(([x, y], index) => index === 0
        ? ctx.moveTo(x * dispW, y * dispH) : ctx.lineTo(x * dispW, y * dispH)); ctx.closePath()
      ctx.strokeStyle = '#22d3ee'; ctx.lineWidth = 3; ctx.stroke()
      ctx.fillStyle = '#67e8f9'; ctx.font = 'bold 12px sans-serif'; ctx.textAlign = 'left'
      ctx.fillText('可绘制业务 ROI 的范围', inferenceRegion.rect.x * dispW + 6,
        Math.max(14, inferenceRegion.rect.y * dispH - 6))
    }

    // 已完成的各区域: 闭合多边形 + 半透明填充 + 顶点 + 标号/名字
    wzones.forEach((z, i) => {
      if (z.pts.length < 2) return
      const col = invalidZoneIndexes.has(i) ? '#ef4444' : ZONE_COLORS[i % ZONE_COLORS.length]
      const px  = z.pts.map(([x, y]) => [x * dispW, y * dispH] as [number, number])
      ctx.beginPath()
      ctx.moveTo(px[0][0], px[0][1])
      for (let k = 1; k < px.length; k++) ctx.lineTo(px[k][0], px[k][1])
      ctx.closePath()
      ctx.fillStyle   = hexToRgba(col, i === sel ? 0.32 : 0.18)
      ctx.fill()
      ctx.strokeStyle = col
      ctx.lineWidth   = i === sel ? 3 : 2
      ctx.stroke()
      // 顶点手柄: 选中的区域放大并描白边, 方便拖拽
      const vr = i === sel ? 6 : 3
      px.forEach(([x, y]) => {
        ctx.beginPath(); ctx.arc(x, y, vr, 0, Math.PI * 2)
        ctx.fillStyle = col; ctx.fill()
        if (i === sel) { ctx.strokeStyle = '#fff'; ctx.lineWidth = 1.5; ctx.stroke() }
      })
      // 标签
      ctx.fillStyle = col
      ctx.font = 'bold 13px sans-serif'
      ctx.textAlign = 'left'
      ctx.fillText(`${i + 1}. ${z.name?.trim() || '区域' + (i + 1)}`, px[0][0] + 6, Math.max(14, px[0][1] - 6))
    })

    // 进行中的草稿
    if (drawing && draft.length > 0) {
      ctx.beginPath()
      ctx.moveTo(draft[0][0], draft[0][1])
      for (let k = 1; k < draft.length; k++) ctx.lineTo(draft[k][0], draft[k][1])
      if (hover) ctx.lineTo(hover[0], hover[1])
      // 绘制过程中用红色(白色在白底画面上看不见)；完成后的各区域仍按 ZONE_COLORS 五颜六色上色
      ctx.strokeStyle = '#ef4444'; ctx.lineWidth = 2; ctx.stroke()
      draft.forEach(([x, y], k) => {
        ctx.beginPath(); ctx.arc(x, y, k === 0 ? 7 : 4, 0, Math.PI * 2)
        ctx.fillStyle = '#ef4444'; ctx.fill()
        if (k === 0) { ctx.strokeStyle = '#7f1d1d'; ctx.lineWidth = 1.5; ctx.stroke() }  // 首点深红描边, 标出闭合点
      })
      // 靠近首点的闭合提示
      if (draft.length >= 3 && hover) {
        const d = Math.hypot(hover[0] - draft[0][0], hover[1] - draft[0][1])
        if (d < SNAP_PX) {
          ctx.beginPath(); ctx.arc(draft[0][0], draft[0][1], SNAP_PX + 4, 0, Math.PI * 2)
          ctx.strokeStyle = '#ef4444'; ctx.lineWidth = 1.5; ctx.stroke()
        }
      }
    }
  }, [wzones, draft, drawing, hover, sel, bgImage, dispW, dispH, inferenceRegion, invalidZoneIndexes])

  useEffect(() => { render() }, [render])
  // eslint-disable-next-line react-hooks/exhaustive-deps
  useEffect(() => { if (streamData) grabFrame() }, [])

  // ── Mouse handlers ──
  const getPos = (e: React.MouseEvent<HTMLCanvasElement>): [number, number] => {
    const r = canvasRef.current!.getBoundingClientRect()
    return [e.clientX - r.left, e.clientY - r.top]
  }

  // 命中测试(显示像素): 找鼠标附近的顶点 / 落在哪个区域内(后画的在上, 优先命中)
  const HIT_R = 9
  const hitVertex = (mx: number, my: number): { zi: number; vi: number } | null => {
    for (let zi = wzones.length - 1; zi >= 0; zi--) {
      const z = wzones[zi]
      for (let vi = 0; vi < z.pts.length; vi++)
        if (Math.hypot(mx - z.pts[vi][0] * dispW, my - z.pts[vi][1] * dispH) <= HIT_R) return { zi, vi }
    }
    return null
  }
  const hitZone = (mx: number, my: number): number => {
    for (let zi = wzones.length - 1; zi >= 0; zi--)
      if (wzones[zi].pts.length >= 3 && pointInPoly(mx / dispW, my / dispH, wzones[zi].pts)) return zi
    return -1
  }

  const commitDraft = (pts: [number, number][]) => {
    if (pts.length < 3) return
    const norm = pts.map(([x, y]) => [+(x / dispW).toFixed(5), +(y / dispH).toFixed(5)] as [number, number])
    if (!zoneAllowed(norm)) return
    setWzones(prev => [...prev, { name: nextDefaultZoneName(prev), pts: norm }])
    setSel(wzones.length)   // 新区域的下标 = 添加前的长度
    setDraft([]); setDrawing(false); setHover(null)
  }
  const clampToAllowed = (x: number, y: number): [number, number] => [
    clamp01(x / dispW) * dispW,
    clamp01(y / dispH) * dispH,
  ]

  const handleMouseDown = (e: React.MouseEvent<HTMLCanvasElement>) => {
    const [x, y] = getPos(e)
    if (drawing) {                                   // 绘制态: 左键加顶点 / 靠近首点闭合
      if (!pointAllowed(x / dispW, y / dispH)) return
      if (draft.length >= 3) {
        const d = Math.hypot(x - draft[0][0], y - draft[0][1])
        if (d < SNAP_PX) { commitDraft(draft); return }
      }
      if (draft.length < 64) setDraft(prev => [...prev, [x, y]])
      return
    }
    // 编辑态: 优先抓顶点(拖拽改形状/大小), 否则抓整块区域(整体移动)
    const v = hitVertex(x, y)
    if (v) { setSel(v.zi); setDrag({ kind: 'vertex', zi: v.zi, vi: v.vi }); return }
    const zi = hitZone(x, y)
    if (zi >= 0) { setSel(zi); setDrag({ kind: 'zone', zi, lastX: x, lastY: y }); return }
    setSel(-1)
  }

  const handleMouseMove = (e: React.MouseEvent<HTMLCanvasElement>) => {
    const [x, y] = getPos(e)
    if (drawing) {
      const [cx, cy] = clampToAllowed(x, y)
      setHover([cx, cy]); return
    }

    if (drag) {
      if (drag.kind === 'vertex') {                  // 拖单个顶点
        const dzi = drag.zi, dvi = drag.vi
        const [cx, cy] = clampToAllowed(x, y)
        const nx = cx / dispW, ny = cy / dispH
        setWzones(prev => prev.map((z, zi) => {
          if (zi !== dzi) return z
          const target = z.pts.map((p, vi) => vi === dvi ? [nx, ny] as [number, number] : p)
          if (zoneAllowed(target)) return { ...z, pts: target }
          // 指针越界时沿拖动轨迹二分到最后一个合法位置，避免顶点突然卡住。
          const origin = z.pts[dvi]
          let low = 0, high = 1, best = z.pts
          for (let step = 0; step < 14; step++) {
            const ratio = (low + high) / 2
            const candidate = z.pts.map((p, vi) => vi === dvi
              ? [origin[0] + (nx - origin[0]) * ratio, origin[1] + (ny - origin[1]) * ratio] as [number, number] : p)
            if (zoneAllowed(candidate)) { low = ratio; best = candidate } else high = ratio
          }
          return { ...z, pts: best }
        }))
      } else {                                        // 整块平移(限幅, 保持形状不出界)
        const dzi = drag.zi, lx = drag.lastX, ly = drag.lastY
        setWzones(prev => prev.map((z, zi) => {
          if (zi !== dzi) return z
          const xs = z.pts.map(p => p[0]), ys = z.pts.map(p => p[1])
          let dx = (x - lx) / dispW, dy = (y - ly) / dispH
          dx = Math.max(-Math.min(...xs), Math.min(1 - Math.max(...xs), dx))
          dy = Math.max(-Math.min(...ys), Math.min(1 - Math.max(...ys), dy))
          const moved = z.pts.map(([px, py]) => [px + dx, py + dy] as [number, number])
          if (zoneAllowed(moved)) return { ...z, pts: moved }
          let low = 0, high = 1, best = z.pts
          for (let step = 0; step < 14; step++) {
            const ratio = (low + high) / 2
            const candidate = z.pts.map(([px, py]) => [px + dx * ratio, py + dy * ratio] as [number, number])
            if (zoneAllowed(candidate)) { low = ratio; best = candidate } else high = ratio
          }
          return { ...z, pts: best }
        }))
        setDrag({ kind: 'zone', zi: dzi, lastX: x, lastY: y })
      }
      return
    }
    // 未拖拽: 命中反馈(驱动光标)
    setHoverHit(hitVertex(x, y) ? 'vertex' : (hitZone(x, y) >= 0 ? 'zone' : null))
  }

  const endDrag = () => setDrag(null)

  const handleContextMenu = (e: React.MouseEvent) => {
    e.preventDefault()
    if (drawing) { setDraft(prev => prev.slice(0, -1)); return }   // 绘制态: 撤销最后一个顶点
    // 编辑态: 右键顶点 → 删除该顶点(至少保留 3 个)
    const [x, y] = getPos(e as React.MouseEvent<HTMLCanvasElement>)
    const v = hitVertex(x, y)
    if (v && wzones[v.zi].pts.length > 3)
      setWzones(prev => prev.map((z, zi) => zi !== v.zi ? z : { ...z, pts: z.pts.filter((_, i) => i !== v.vi) }))
  }

  // ── Sidebar ops ──
  const startNewZone = () => { setDrawing(true); setDraft([]); setHover(null) }
  const cancelDraft  = () => { setDrawing(false); setDraft([]); setHover(null) }
  const renameZone   = (i: number, name: string) =>
    setWzones(prev => prev.map((z, k) => k === i ? { ...z, name } : z))
  const deleteZone   = (i: number) => {
    setWzones(prev => prev.filter((_, k) => k !== i))
    setSel(-1)
  }

  const handleSave = () => {
    if (duplicateNames.size > 0 || invalidZoneIndexes.size > 0) return
    const out: Zone[] = wzones
      .filter(z => z.pts.length >= 3)
      .map(z => ({
        name: z.name.trim(),
        polygon: normalizeRoiPolygon(z.pts.map(([x, y]) => [x, y])),
      }))
    if (out.length === 0) clearZones(nodeId)
    else                  setZones(nodeId, out, srcW, srcH)
    onClose()
  }

  const isUsb = streamData ? getSrcType(streamData) === 'usb' : false
  const normalizedDraft = draft.map(([x, y]) => [x / dispW, y / dispH] as [number, number])
  const draftCanClose = normalizedDraft.length >= 3 && zoneAllowed(normalizedDraft)

  // 画布光标: 绘制态十字; 编辑态按"悬停/拖拽顶点=抓手, 区域=移动"
  const canvasCursor = drawing ? 'crosshair'
    : drag ? (drag.kind === 'vertex' ? 'grabbing' : 'move')
    : hoverHit === 'vertex' ? 'grab'
    : hoverHit === 'zone' ? 'move'
    : 'crosshair'

  return (
    // 点击空白遮罩不关闭——避免画/编辑到一半误触外侧丢失未保存的区域；只能用 ✕ / 保存 关闭。
    <div className="roi-overlay">
      <div className="roi-dialog">
        {/* Header */}
        <div className="roi-hdr">
          <span>业务 ROI 绘制（可画多个区域，各自命名）</span>
          <div className="roi-hdr-actions">
            <button className="roi-grab-btn" onClick={grabFrame} disabled={loading}>
              {loading ? '抓取中…' : '📷 抓取当前帧'}
            </button>
            {snapErr && <span className="roi-err">{snapErr}</span>}
            {srcW > 0 && <span className="roi-res">{srcW}×{srcH}</span>}
            <button className="roi-close-btn" onClick={onClose}>✕</button>
          </div>
        </div>

        {isUsb && (
          Number(streamData!.usb_width) > 0 ? (
            <div style={{ padding: '6px 12px', background: '#12321a', color: '#34d399', fontSize: 12, lineHeight: 1.5 }}>
              ✓ USB 采集分辨率已固定为 {Number(streamData!.usb_width)}×{Number(streamData!.usb_height)}（显式配置，不随最大FPS变）。在此分辨率下画 ROI 即可。
            </div>
          ) : (
            <div style={{ padding: '6px 12px', background: '#3a2a12', color: '#fbbf24', fontSize: 12, lineHeight: 1.5 }}>
              ⚠ 自动分辨率会随最大 FPS 变化；修改 FPS 后请重新抓帧。
            </div>
          )
        )}

        {inferenceRegion && (
          <div className="inference-roi-help">
            业务 ROI 必须完全位于青色推理区域内。
          </div>
        )}

        {/* Body: canvas + sidebar */}
        <div className="roi-body">
          <div className="roi-canvas-wrap">
            <canvas
              ref={canvasRef}
              width={dispW}
              height={dispH}
              style={{ width: dispW, height: dispH, cursor: canvasCursor }}
              className="roi-canvas"
              onMouseDown={handleMouseDown}
              onMouseMove={handleMouseMove}
              onMouseUp={endDrag}
              onMouseLeave={() => { endDrag(); setHover(null); setHoverHit(null) }}
              onContextMenu={handleContextMenu}
            />
          </div>

          <div className="roi-sidebar">
            <div className="roi-sidebar-title">区域列表（{wzones.length}）</div>
            <div className="roi-sidebar-list">
              {wzones.length === 0 && !drawing && (
                <div className="roi-sidebar-empty">还没有区域，点下方「新增区域」开始绘制</div>
              )}
              {wzones.map((z, i) => (
                <div
                  key={i}
                  className={`roi-zone-item${i === sel ? ' sel' : ''}`}
                  onMouseEnter={() => setSel(i)}
                >
                  <span className="roi-zone-dot" style={{ background: ZONE_COLORS[i % ZONE_COLORS.length] }} />
                  <input
                    className={`roi-zone-input${duplicateNames.has(z.name.trim()) ? ' duplicate' : ''}`}
                    value={z.name}
                    placeholder={`区域${i + 1}`}
                    onChange={e => renameZone(i, e.target.value)}
                    aria-invalid={duplicateNames.has(z.name.trim())}
                    title={duplicateNames.has(z.name.trim()) ? 'ROI 区域名称不能重复' : undefined}
                  />
                  <span className="roi-zone-pts">{z.pts.length}点</span>
                  <button className="roi-zone-del" title="删除该区域" onClick={() => deleteZone(i)}>🗑</button>
                </div>
                ))}
            </div>

            {duplicateNames.size > 0 && (
              <div className="roi-name-error">
                ROI 区域名称不能重复：{[...duplicateNames].join('、')}。请修改后再保存。
              </div>
            )}
            {invalidZoneIndexes.size > 0 && (
              <div className="roi-name-error">
                红色区域无效、自相交或穿出了推理 ROI，请调整顶点或整个区域。
              </div>
            )}

            {drawing ? (
              <div className="roi-draw-hint">
                绘制中：左键加顶点（{draft.length}）· 靠近首点或点「完成」闭合 · 右键撤销
                {draft.length >= 3 && !draftCanClose && <div className="roi-name-error">
                  当前边界自相交、面积无效或穿出了推理 ROI，请继续调整顶点。
                </div>}
                <div className="roi-draw-actions">
                  <button className="roi-btn primary" disabled={!draftCanClose} onClick={() => commitDraft(draft)}>完成此区域</button>
                  <button className="roi-btn" onClick={cancelDraft}>取消</button>
                </div>
              </div>
            ) : (
              <>
                {wzones.length > 0 && (
                  <div className="roi-edit-tip">拖顶点改形状 · 拖区域内部移动 · 右键顶点删点</div>
                )}
                <button className="roi-btn primary roi-add-btn" onClick={startNewZone}>＋ 新增区域</button>
              </>
            )}
          </div>
        </div>

        {/* Footer */}
        <div className="roi-footer">
          <span />
          <div className="roi-actions">
            <button
              className="roi-btn primary"
              onClick={handleSave}
              disabled={duplicateNames.size > 0 || invalidZoneIndexes.size > 0}
              title={duplicateNames.size > 0 ? '请先修改重复的 ROI 区域名称'
                : invalidZoneIndexes.size > 0 ? '业务 ROI 不能超出推理 ROI' : undefined}
            >
              保存
            </button>
          </div>
        </div>
      </div>
    </div>
  )
}

// #rrggbb + alpha → rgba()
function hexToRgba(hex: string, a: number): string {
  const m = hex.replace('#', '')
  const r = parseInt(m.slice(0, 2), 16)
  const g = parseInt(m.slice(2, 4), 16)
  const b = parseInt(m.slice(4, 6), 16)
  return `rgba(${r},${g},${b},${a})`
}
