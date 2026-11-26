import { useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'

interface Props {
  name: string
  original: string
  inference?: string
  inferenceLabel: string
}

const initialView = { scale: 1, x: 0, y: 0 }

function boundPan(offset: number, scale: number) {
  const limit = (scale - 1) / 2
  return Math.max(-limit, Math.min(limit, offset))
}

export default function ImageComparison({ name, original, inference, inferenceLabel }: Props) {
  const [expanded, setExpanded] = useState(false)
  const [view, setView] = useState(initialView)
  const dialog = useRef<HTMLDialogElement>(null)
  const drag = useRef<{ id: number; x: number; y: number; startX: number; startY: number } | null>(null)
  const images = [
    { id: 'original', label: '原图', src: original },
    { id: 'inference', label: inferenceLabel, src: inference },
  ]
  useEffect(() => {
    if (!expanded) return
    const element = dialog.current
    element?.showModal()
    return () => { element?.close() }
  }, [expanded])

  const zoom = (factor: number) => setView(previous => {
    const scale = Math.min(8, Math.max(1, previous.scale * factor))
    return { scale, x: boundPan(previous.x * scale / previous.scale, scale),
      y: boundPan(previous.y * scale / previous.scale, scale) }
  })

  return <>
    <div className="image-compare" aria-label="原图与推理图对照">
      {images.map(({ id, label, src }) => <figure key={id}>
        <figcaption>{label}</figcaption>
        <button className="image-stage" data-view={id} disabled={!src} aria-label={`放大查看${label}`}
          onClick={() => { setView(initialView); setExpanded(true) }}>
          {src && <img src={src} alt={`${name} · ${label}`} />}
        </button>
      </figure>)}
    </div>
    {expanded && createPortal(<dialog ref={dialog} className="image-comparison-dialog"
      aria-label="原图与推理图联动对照" onCancel={() => setExpanded(false)} onKeyDown={event => {
        if (event.key === '+' || event.key === '=') zoom(2)
        else if (event.key === '-') zoom(0.5)
        else if (event.key === '0') setView(initialView)
      }}>
      <div className="comparison-header"><strong>{name}</strong>
        <div className="comparison-controls">
          <button disabled={view.scale <= 1} onClick={() => zoom(0.5)}>缩小</button>
          <output>{view.scale * 100}%</output>
          <button disabled={view.scale >= 8} onClick={() => zoom(2)}>放大</button>
          <button onClick={() => setView(initialView)}>复位</button>
          <button autoFocus onClick={() => setExpanded(false)}>关闭</button>
        </div>
      </div>
      <div className="comparison-panes">
        {images.map(({ id, label, src }) => <figure key={id}>
          <figcaption>{label}</figcaption>
          <div className="comparison-pan" data-zoomed={view.scale > 1}
            onPointerDown={event => {
              if (view.scale <= 1 || !event.isPrimary || event.button !== 0) return
              drag.current = { id: event.pointerId, x: view.x, y: view.y, startX: event.clientX, startY: event.clientY }
              event.currentTarget.setPointerCapture(event.pointerId)
            }}
            onPointerMove={event => {
              const start = drag.current
              if (!start || start.id !== event.pointerId) return
              const { width, height } = event.currentTarget.getBoundingClientRect()
              setView({ scale: view.scale, x: boundPan(start.x + (event.clientX - start.startX) / width, view.scale),
                y: boundPan(start.y + (event.clientY - start.startY) / height, view.scale) })
            }}
            onLostPointerCapture={() => { drag.current = null }}>
            {src && <img draggable={false} src={src} alt={label}
              style={{ transform: `translate(${view.x * 100}%, ${view.y * 100}%) scale(${view.scale})` }} />}
          </div>
        </figure>)}
      </div>
    </dialog>, document.body)}
  </>
}
