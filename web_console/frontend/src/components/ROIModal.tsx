import { useRef, useEffect, useState, useCallback } from 'react'
import { createPortal } from 'react-dom'
import { useROIStore } from '../store/roiStore'
import './ROIModal.css'

const EMPTY_ROI: number[][] = []

const DISPLAY_W = 960
const DISPLAY_H = 540
const SNAP_DIST = 10

const RESOLUTIONS = [
  { label: '1920×1080', w: 1920, h: 1080 },
  { label: '1280×720',  w: 1280, h: 720  },
  { label: '640×480',   w: 640,  h: 480  },
  { label: '3840×2160', w: 3840, h: 2160 },
]

type DrawState = 'idle' | 'drawing' | 'closed'

interface Props {
  nodeId: string
  onClose: () => void
}

export default function ROIModal({ nodeId, onClose }: Props) {
  // 旧版单区域弹窗(未在画布启用): 映射到新版 zones API 的第一个区域, 保持可编译/可用
  const existingPolygon = useROIStore(s => s.zones[nodeId]?.[0]?.polygon ?? EMPTY_ROI)
  const storeSetZones   = useROIStore(s => s.setZones)
  const setPolygon = (id: string, poly: number[][]) =>
    storeSetZones(id, poly.length >= 3 ? [{ name: '', polygon: poly }] : [])

  const canvasRef = useRef<HTMLCanvasElement>(null)
  const [resIdx, setResIdx] = useState(0)

  const res = RESOLUTIONS[resIdx]
  const scaleX = res.w / DISPLAY_W
  const scaleY = res.h / DISPLAY_H

  // Convert existing source-space points to display-space for initial state
  const initPoints = (): [number, number][] => {
    if (existingPolygon.length === 0) return []
    const pts = existingPolygon.map(([x, y]): [number, number] => [x / scaleX, y / scaleY])
    // If last point === first point (closed polygon), remove the duplicate
    if (pts.length > 1) {
      const last = pts[pts.length - 1]
      const first = pts[0]
      if (Math.abs(last[0] - first[0]) < 1 && Math.abs(last[1] - first[1]) < 1) {
        pts.pop()
      }
    }
    return pts
  }

  const [points, setPoints] = useState<[number, number][]>(() => initPoints())
  const [drawState, setDrawState] = useState<DrawState>(existingPolygon.length >= 3 ? 'closed' : 'idle')
  const [hover, setHover] = useState<[number, number] | null>(null)

  const render = useCallback(() => {
    const canvas = canvasRef.current
    if (!canvas) return
    const ctx = canvas.getContext('2d')!
    ctx.clearRect(0, 0, DISPLAY_W, DISPLAY_H)
    if (points.length === 0) return

    ctx.beginPath()
    ctx.moveTo(points[0][0], points[0][1])
    for (let i = 1; i < points.length; i++) ctx.lineTo(points[i][0], points[i][1])

    if (drawState === 'closed') {
      ctx.closePath()
      ctx.fillStyle = 'rgba(251,146,60,0.25)'
      ctx.fill()
      ctx.strokeStyle = '#f97316'
      ctx.lineWidth = 2
      ctx.stroke()
    } else {
      if (hover) ctx.lineTo(hover[0], hover[1])
      ctx.strokeStyle = '#fbbf24'
      ctx.lineWidth = 2
      ctx.stroke()
    }

    // Vertex dots
    points.forEach(([x, y], i) => {
      ctx.beginPath()
      ctx.arc(x, y, i === 0 ? 7 : 4, 0, Math.PI * 2)
      ctx.fillStyle = i === 0 ? '#ef4444' : '#fbbf24'
      ctx.fill()
    })
  }, [points, drawState, hover])

  useEffect(() => { render() }, [render])

  // Re-init points when resolution changes
  useEffect(() => {
    const pts = initPoints()
    setPoints(pts)
    setDrawState(pts.length >= 3 ? 'closed' : pts.length > 0 ? 'drawing' : 'idle')
    setHover(null)
  }, [resIdx]) // eslint-disable-line

  const handleMouseDown = (e: React.MouseEvent<HTMLCanvasElement>) => {
    if (drawState === 'closed') return
    const rect = canvasRef.current!.getBoundingClientRect()
    const x = e.clientX - rect.left
    const y = e.clientY - rect.top

    if (drawState === 'idle') {
      setPoints([[x, y]])
      setDrawState('drawing')
      return
    }

    // Check close distance
    if (points.length >= 3) {
      const [fx, fy] = points[0]
      const dist = Math.hypot(x - fx, y - fy)
      if (dist < SNAP_DIST) {
        setDrawState('closed')
        setHover(null)
        return
      }
    }
    setPoints(prev => [...prev, [x, y]])
  }

  const handleMouseMove = (e: React.MouseEvent<HTMLCanvasElement>) => {
    if (drawState !== 'drawing') return
    const rect = canvasRef.current!.getBoundingClientRect()
    setHover([e.clientX - rect.left, e.clientY - rect.top])
  }

  const handleContextMenu = (e: React.MouseEvent) => {
    e.preventDefault()
    if (drawState === 'drawing' && points.length > 0) {
      setPoints(prev => {
        const next = prev.slice(0, -1)
        if (next.length === 0) setDrawState('idle')
        return next
      })
    }
  }

  const handleClear = () => {
    setPoints([])
    setDrawState('idle')
    setHover(null)
  }

  const handleUndo = () => {
    if (drawState === 'closed') {
      setDrawState('drawing')
      return
    }
    setPoints(prev => {
      const next = prev.slice(0, -1)
      if (next.length === 0) setDrawState('idle')
      return next
    })
  }

  const handleSave = () => {
    if (points.length < 3) {
      // Save empty polygon
      setPolygon(nodeId, [])
      onClose()
      return
    }
    // Convert display-space → source-space and close polygon
    const srcPts = points.map(([x, y]): number[] => [Math.round(x * scaleX), Math.round(y * scaleY)])
    // Close polygon (append first point)
    srcPts.push([...srcPts[0]])
    setPolygon(nodeId, srcPts)
    onClose()
  }

  const snapIndicator = drawState === 'drawing' && points.length >= 3 && hover
    ? Math.hypot(hover[0] - points[0][0], hover[1] - points[0][1]) < SNAP_DIST
    : false

  const modal = (
    <div className="roi-overlay" onClick={e => { if (e.target === e.currentTarget) onClose() }}>
      <div className="roi-dialog">
        <div className="roi-header">
          <span>ROI 区域绘制</span>
          <div className="roi-res-select">
            <label>源分辨率：</label>
            <select value={resIdx} onChange={e => setResIdx(+e.target.value)}>
              {RESOLUTIONS.map((r, i) => <option key={i} value={i}>{r.label}</option>)}
            </select>
          </div>
          <button className="roi-close" onClick={onClose}>✕</button>
        </div>

        <div className="roi-canvas-wrap">
          <canvas
            ref={canvasRef}
            width={DISPLAY_W}
            height={DISPLAY_H}
            className={`roi-canvas${drawState === 'drawing' ? ' drawing' : ''}`}
            onMouseDown={handleMouseDown}
            onMouseMove={handleMouseMove}
            onContextMenu={handleContextMenu}
          />
          {snapIndicator && (
            <div className="roi-snap-hint">松开以闭合多边形</div>
          )}
        </div>

        <div className="roi-footer">
          <div className="roi-hint">
            {drawState === 'idle' && '左键点击开始绘制多边形顶点'}
            {drawState === 'drawing' && `已绘 ${points.length} 个顶点 · 右键撤销 · 点击首点闭合`}
            {drawState === 'closed' && `已闭合（${points.length} 个顶点）`}
          </div>
          <div className="roi-actions">
            <button className="roi-btn" onClick={handleUndo}>撤销</button>
            <button className="roi-btn" onClick={handleClear}>清除</button>
            <button className="roi-btn danger" onClick={() => { setPolygon(nodeId, []); onClose() }}>删除 ROI</button>
            <button className="roi-btn primary" onClick={handleSave}>保存</button>
          </div>
        </div>
      </div>
    </div>
  )
  return createPortal(modal, document.body)
}
