import { Handle, Position, NodeProps, useReactFlow } from '@xyflow/react'
import { useState, useRef, useEffect, useCallback } from 'react'
import { createPortal } from 'react-dom'
import { useROIStore, type Zone } from '../store/roiStore'
import { useEditorStore } from '../store/editorStore'
import { captureSnapshot } from '../api/client'
import { getSrcType } from '../utils/streamSource'
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

// 归一化多边形(存储为闭合: 首尾点相同) → 去掉重复的闭合点, 得到不重复顶点
function stripClose(poly: number[][]): [number, number][] {
  if (poly.length >= 2) {
    const a = poly[0], b = poly[poly.length - 1]
    if (Math.abs(a[0] - b[0]) < 1e-6 && Math.abs(a[1] - b[1]) < 1e-6)
      return poly.slice(0, -1).map(p => [p[0], p[1]] as [number, number])
  }
  return poly.map(p => [p[0], p[1]] as [number, number])
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

  // ── Get connected stream info + USB resolution hint by traversing the graph ──
  const getStreamInfo = (): {
    streamData: Record<string, unknown> | null
    usbRes: { width: number; height: number } | null
  } => {
    const edges = rf.getEdges()
    // ROI 可接到 YOLO 模型节点(经 stream-in 取流) 或 传统通道里被视频流直连的逻辑节点(经 logic-in 取流)。
    const toAnchor = edges.find(e => e.source === id && e.sourceHandle === 'roi-out')
    if (!toAnchor) return { streamData: null, usbRes: null }
    const toStream = edges.find(e =>
      e.target === toAnchor.target &&
      (e.targetHandle === 'stream-in' || e.targetHandle === 'logic-in') &&
      rf.getNode(e.source)?.type === 'stream')
    if (!toStream) return { streamData: null, usbRes: null }
    const streamData = (rf.getNode(toStream.source)?.data as Record<string, unknown>) ?? null

    let usbRes: { width: number; height: number } | null = null
    if (getSrcType(streamData) === 'usb') {
      const ew = Number(streamData.usb_width ?? 0)
      const eh = Number(streamData.usb_height ?? 0)
      if (ew > 0 && eh > 0) {
        usbRes = { width: ew, height: eh }
      } else {
        // 锚点可能是 model(带 playback_fps/max_fps) 或 logic(无 → 回退全局最大FPS)
        const anchorData = rf.getNode(toAnchor.target)?.data as Record<string, unknown> | undefined
        const playbackFps = Number(anchorData?.playback_fps ?? 0)
        const maxFps      = Number(anchorData?.max_fps      ?? 0)
        const fps = playbackFps > 0 ? playbackFps
                  : maxFps      > 0 ? maxFps
                  : globalMaxFps > 0 ? globalMaxFps
                  : 15
        usbRes = usbResolutionForFps(fps)
      }
    }
    return { streamData, usbRes }
  }

  return (
    <>
      <div className={`rf-node${selected ? ' selected' : ''}`} style={{ minWidth: 210 }}>
        <div className="rf-node-header header-roi">
          <span>⬡</span><span>ROI 检测区域</span>
          {nZones > 0 && <span className="roi-count-badge">{nZones}</span>}
        </div>

        <div className="rf-node-body">
          <div className={`roi-chip${nZones > 0 ? ' active' : ''}`}>
            {nZones > 0 ? `✔ 已配置 ${nZones} 个区域` : '🔲 未配置 → 全屏检测'}
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
            {nZones > 0 ? '编辑 ROI 区域' : '绘制 ROI 区域'}
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
  onClose: () => void
}

// 编辑期内部表示: 顶点为归一化(0~1)、不含闭合重复点
interface WZone { name: string; pts: [number, number][] }

// 编辑(非绘制)态的拖拽: 拖某顶点, 或整块移动某区域
type DragState =
  | { kind: 'vertex'; zi: number; vi: number }
  | { kind: 'zone'; zi: number; lastX: number; lastY: number }
  | null

function ROIDrawModal({ nodeId, appName, streamData, usbRes, onClose }: ModalProps) {
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
        device: String(streamData.device ?? '/dev/video0'),
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

    // 已完成的各区域: 闭合多边形 + 半透明填充 + 顶点 + 标号/名字
    wzones.forEach((z, i) => {
      if (z.pts.length < 2) return
      const col = ZONE_COLORS[i % ZONE_COLORS.length]
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
  }, [wzones, draft, drawing, hover, sel, bgImage, dispW, dispH])

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
    setWzones(prev => [...prev, { name: `区域${prev.length + 1}`, pts: norm }])
    setSel(wzones.length)   // 新区域的下标 = 添加前的长度
    setDraft([]); setDrawing(false); setHover(null)
  }

  const handleMouseDown = (e: React.MouseEvent<HTMLCanvasElement>) => {
    const [x, y] = getPos(e)
    if (drawing) {                                   // 绘制态: 左键加顶点 / 靠近首点闭合
      if (draft.length >= 3) {
        const d = Math.hypot(x - draft[0][0], y - draft[0][1])
        if (d < SNAP_PX) { commitDraft(draft); return }
      }
      setDraft(prev => [...prev, [x, y]])
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
    if (drawing) { setHover([x, y]); return }

    if (drag) {
      if (drag.kind === 'vertex') {                  // 拖单个顶点
        const dzi = drag.zi, dvi = drag.vi
        const nx = clamp01(x / dispW), ny = clamp01(y / dispH)
        setWzones(prev => prev.map((z, zi) => zi !== dzi ? z
          : { ...z, pts: z.pts.map((p, vi) => vi === dvi ? [nx, ny] as [number, number] : p) }))
      } else {                                        // 整块平移(限幅, 保持形状不出界)
        const dzi = drag.zi, lx = drag.lastX, ly = drag.lastY
        setWzones(prev => prev.map((z, zi) => {
          if (zi !== dzi) return z
          const xs = z.pts.map(p => p[0]), ys = z.pts.map(p => p[1])
          let dx = (x - lx) / dispW, dy = (y - ly) / dispH
          dx = Math.max(-Math.min(...xs), Math.min(1 - Math.max(...xs), dx))
          dy = Math.max(-Math.min(...ys), Math.min(1 - Math.max(...ys), dy))
          return { ...z, pts: z.pts.map(([px, py]) => [px + dx, py + dy] as [number, number]) }
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
    const out: Zone[] = wzones
      .filter(z => z.pts.length >= 3)
      .map(z => {
        const poly = z.pts.map(([x, y]) => [x, y] as number[])
        poly.push([...poly[0]])  // 闭合(首尾相同), 与 C++/旧格式一致
        return { name: z.name.trim(), polygon: poly }
      })
    if (out.length === 0) clearZones(nodeId)
    else                  setZones(nodeId, out, srcW, srcH)
    onClose()
  }

  const isUsb = streamData ? getSrcType(streamData) === 'usb' : false

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
          <span>ROI 区域绘制（可画多个区域，各自命名）</span>
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
              ⚠ 当前为「自动」分辨率：采集视野随「全局最大FPS」变化（当前抓帧 {srcW}×{srcH}），改 FPS 后需重抓帧。建议到「视频流节点」把「采集分辨率」设为固定值。
            </div>
          )
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
                    className="roi-zone-input"
                    value={z.name}
                    placeholder={`区域${i + 1}`}
                    onChange={e => renameZone(i, e.target.value)}
                  />
                  <span className="roi-zone-pts">{z.pts.length}点</span>
                  <button className="roi-zone-del" title="删除该区域" onClick={() => deleteZone(i)}>🗑</button>
                </div>
              ))}
            </div>

            {drawing ? (
              <div className="roi-draw-hint">
                绘制中：左键加顶点（{draft.length}）· 靠近首点或点「完成」闭合 · 右键撤销
                <div className="roi-draw-actions">
                  <button className="roi-btn primary" disabled={draft.length < 3} onClick={() => commitDraft(draft)}>完成此区域</button>
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
          <span className="roi-hint">
            拖顶点改形状/大小 · 拖区域内部整体移动 · 右键顶点删点；坐标按比例存储，与分辨率解耦。
          </span>
          <div className="roi-actions">
            <button className="roi-btn primary" onClick={handleSave}>保存</button>
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
