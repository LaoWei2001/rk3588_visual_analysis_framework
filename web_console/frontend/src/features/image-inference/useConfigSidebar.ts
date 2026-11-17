import { useCallback, useEffect, useRef, useState, type HTMLAttributes, type KeyboardEvent, type PointerEvent } from 'react'

const MIN_WIDTH = 240
const MAX_WIDTH = 520
const COLLAPSE_WIDTH = 160
const RESULT_MIN_WIDTH = 420

interface Drag {
  pointerId: number
  element: HTMLDivElement
  startX: number
  startWidth: number
  cursor: string
  userSelect: string
}

export default function useConfigSidebar() {
  const [collapsed, setCollapsed] = useState(false)
  const [preferredWidth, setPreferredWidth] = useState(300)
  const [maxWidth, setMaxWidth] = useState(MAX_WIDTH)
  const [resizing, setResizing] = useState(false)
  const workspaceRef = useRef<HTMLDivElement>(null)
  const dragRef = useRef<Drag | null>(null)
  const width = Math.min(preferredWidth, maxWidth)

  const finish = useCallback((updateState = true) => {
    const drag = dragRef.current
    if (!drag) return
    dragRef.current = null
    document.body.style.cursor = drag.cursor
    document.body.style.userSelect = drag.userSelect
    if (drag.element.hasPointerCapture(drag.pointerId)) drag.element.releasePointerCapture(drag.pointerId)
    if (updateState) setResizing(false)
  }, [])

  useEffect(() => {
    const workspace = workspaceRef.current
    if (!workspace) return
    const measure = () => {
      if (window.innerWidth <= 1000) { finish(); return }
      const gap = parseFloat(getComputedStyle(workspace).columnGap) || 0
      setMaxWidth(Math.min(MAX_WIDTH, Math.max(MIN_WIDTH, workspace.clientWidth - gap - RESULT_MIN_WIDTH)))
    }
    const blur = () => finish()
    const observer = new ResizeObserver(measure)
    observer.observe(workspace)
    window.addEventListener('resize', measure)
    window.addEventListener('blur', blur)
    measure()
    return () => {
      observer.disconnect()
      window.removeEventListener('resize', measure)
      window.removeEventListener('blur', blur)
      finish(false)
    }
  }, [finish])

  const resize = (next: number) => {
    if (next < COLLAPSE_WIDTH) { setCollapsed(true); return }
    setPreferredWidth(Math.round(Math.min(maxWidth, Math.max(MIN_WIDTH, next))))
    setCollapsed(false)
  }
  const begin = (event: PointerEvent<HTMLDivElement>) => {
    if (event.button !== 0 || !event.isPrimary || dragRef.current) return
    event.preventDefault()
    event.currentTarget.focus({ preventScroll: true })
    event.currentTarget.setPointerCapture(event.pointerId)
    dragRef.current = {
      pointerId: event.pointerId, element: event.currentTarget,
      startX: event.clientX, startWidth: collapsed ? 0 : width,
      cursor: document.body.style.cursor, userSelect: document.body.style.userSelect,
    }
    document.body.style.cursor = 'col-resize'
    document.body.style.userSelect = 'none'
    setResizing(true)
  }
  const move = (event: PointerEvent<HTMLDivElement>) => {
    const drag = dragRef.current
    if (drag?.pointerId === event.pointerId) resize(drag.startWidth + event.clientX - drag.startX)
  }
  const end = (event: PointerEvent<HTMLDivElement>) => {
    if (dragRef.current?.pointerId === event.pointerId) finish()
  }
  const toggle = () => setCollapsed(value => !value)
  const keyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    if (!['ArrowLeft', 'ArrowRight', 'Home', 'End', 'Enter'].includes(event.key)) return
    event.preventDefault()
    if (event.key === 'Enter') { toggle(); return }
    if (event.key === 'Home') { setCollapsed(true); return }
    if (event.key === 'End') { resize(maxWidth); return }
    if (collapsed) { if (event.key === 'ArrowRight') setCollapsed(false); return }
    const next = width + (event.key === 'ArrowRight' ? 1 : -1) * (event.shiftKey ? 40 : 20)
    resize(next < MIN_WIDTH ? 0 : next)
  }
  const separatorProps: HTMLAttributes<HTMLDivElement> = {
    role: 'separator', tabIndex: 0, title: '拖动调整宽度，向左拖动收起',
    'aria-label': '调整模型配置宽度', 'aria-controls': 'image-inference-config',
    'aria-orientation': 'vertical', 'aria-valuemin': 0, 'aria-valuemax': Math.round(maxWidth),
    'aria-valuenow': collapsed ? 0 : width, 'aria-valuetext': collapsed ? '已收起' : `${width} 像素`,
    onPointerDown: begin, onPointerMove: move, onPointerUp: end,
    onPointerCancel: end, onLostPointerCapture: end, onKeyDown: keyDown,
  }
  return { collapsed, width, resizing, workspaceRef, separatorProps, toggle }
}
