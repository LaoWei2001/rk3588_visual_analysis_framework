import { useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import './TerminalPage.css'
import {
  MAX_TERMINALS, listTerminals, openTerminal, closeTerminal, getSession,
} from './terminalSession'

const FONT_SIZES = [10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24]
const DEFAULT_FONT_SIZE = 20  // 默认初始字号；用户改过后记住

function loadFontSize(): number {
  try {
    const v = localStorage.getItem('terminal-fontSize')
    if (v != null) {
      const n = JSON.parse(v)
      if (typeof n === 'number' && FONT_SIZES.includes(n)) return n
    }
  } catch { /* ignore */ }
  return DEFAULT_FONT_SIZE
}

// ── 单个分屏：把对应会话的 holder 接进自己的容器；卸载时只摘下不销毁(会话续命) ──
function TerminalPane({ id, fontSize, index, canClose, onClose }: {
  id: string; fontSize: number; index: number; canClose: boolean; onClose: () => void
}) {
  const bodyRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    const sess = getSession(id)
    const container = bodyRef.current
    if (!sess || !container) return
    const fit = () => { try { sess.fit.fit() } catch { /* ignore */ } }

    container.appendChild(sess.holder)
    if (!sess.opened) {
      sess.term.open(sess.holder)   // holder 已在 DOM 里，字符尺寸量得准
      sess.opened = true
    }
    sess.term.options.fontSize = fontSize
    fit()
    sess.term.focus()
    if (sess.ws.readyState === WebSocket.OPEN) {
      sess.ws.send(`\x1b[resize:${sess.term.rows}x${sess.term.cols}`)
    }

    // 容器尺寸一变（新增/关闭分屏、窗口缩放、首帧布局稳定）就重新 fit → 同步给 PTY
    let rafId = 0
    const ro = new ResizeObserver(() => {
      cancelAnimationFrame(rafId)
      rafId = requestAnimationFrame(fit)
    })
    ro.observe(container)
    document.fonts?.ready.then(fit).catch(() => {})

    return () => {
      cancelAnimationFrame(rafId)
      ro.disconnect()
      sess.holder.remove()  // 只摘出 DOM，不关 ws、不 dispose
    }
  }, [id])

  // 字号变化：应用到本会话并重排
  useEffect(() => {
    const sess = getSession(id)
    if (!sess) return
    sess.term.options.fontSize = fontSize
    try { sess.fit.fit() } catch { /* ignore */ }
  }, [id, fontSize])

  return (
    <div className="term-pane">
      <div className="term-pane-bar">
        <span className="term-pane-title">终端 {index + 1}</span>
        <button
          className="term-pane-close"
          title={canClose ? '关闭此终端' : '至少保留一个终端'}
          disabled={!canClose}
          onClick={onClose}
        >✕</button>
      </div>
      <div
        className="term-pane-body"
        ref={bodyRef}
        onMouseDown={() => getSession(id)?.term.focus()}
      />
    </div>
  )
}

export default function TerminalPage() {
  const navigate = useNavigate()
  const [fontSize, setFontSize] = useState<number>(loadFontSize)
  const [ids, setIds] = useState<string[]>(listTerminals)

  // 首次进入若一个终端都没有，自动建一个
  useEffect(() => {
    if (listTerminals().length === 0) {
      openTerminal(loadFontSize())
      setIds(listTerminals())
    }
  }, [])

  const addTerminal = () => {
    if (openTerminal(fontSize) !== null) setIds(listTerminals())
  }
  const removeTerminal = (id: string) => {
    closeTerminal(id)
    setIds(listTerminals())
  }

  // 等分网格：列数 = ceil(sqrt(n))，行数随之
  const n = Math.max(ids.length, 1)
  const cols = Math.ceil(Math.sqrt(n))
  const rows = Math.ceil(n / cols)

  return (
    <div className="terminal-page">
      <div className="terminal-header">
        <div className="terminal-header-left">
          <button className="tb-btn" onClick={() => navigate('/')}>返回</button>
          <span className="terminal-title">板端终端</span>
          <button className="tb-btn" onClick={addTerminal} disabled={ids.length >= MAX_TERMINALS}>
            ＋ 新建分屏
          </button>
          <span className="terminal-hint">{ids.length}/{MAX_TERMINALS}</span>
        </div>
        <div className="terminal-header-right">
          <span className="terminal-select-label">字号设置</span>
          <select className="terminal-select" value={fontSize} onChange={e => {
            const v = +e.target.value
            setFontSize(v)
            try { localStorage.setItem('terminal-fontSize', JSON.stringify(v)) } catch { /* ignore */ }
          }}>
            {FONT_SIZES.map(s => <option key={s} value={s}>{s}px</option>)}
          </select>
          <span className="terminal-hint">Ctrl+C / Ctrl+D</span>
        </div>
      </div>

      <div
        className="terminal-grid"
        style={{
          gridTemplateColumns: `repeat(${cols}, minmax(0, 1fr))`,
          gridTemplateRows: `repeat(${rows}, minmax(0, 1fr))`,
        }}
      >
        {ids.map((id, i) => (
          <TerminalPane
            key={id}
            id={id}
            index={i}
            fontSize={fontSize}
            canClose={ids.length > 1}
            onClose={() => removeTerminal(id)}
          />
        ))}
      </div>
    </div>
  )
}
