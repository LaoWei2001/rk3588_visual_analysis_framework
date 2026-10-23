import { useEffect, useRef, useState } from 'react'
import type { CSSProperties } from 'react'
import './TerminalPage.css'
import TerminalCurvature from './TerminalCurvature'
import { SCREEN_RECESS } from './terminalBezelGeometry'
import TerminalSignal from './TerminalSignal'
import { FONT_EVENT } from './terminalSignal'
import { loadTerminalFont, resolveTerminalFont } from './terminalFonts'
import { terminalSnowTexture, useTerminalPhosphor } from './terminalPhosphor'
import { VIGNETTE_START, vignetteOpacity, scanlineOpacity, afterglowOpacity, terminalLightHalos, terminalDotTexture } from './terminalOptics'
import {
  MAX_TERMINALS, listTerminals, openTerminal, closeTerminal, getSession,
} from './terminalSession'
import {
  DEFAULT_APPEARANCE, TERMINAL_PROFILES, TERMINAL_FONTS, loadTerminalAppearance,
  saveTerminalAppearance, terminalAppearanceOptions,
} from './terminalAppearance'
import type { TerminalAppearance, TerminalProfile, TerminalFont } from './terminalAppearance'

const MIN_FONT_SIZE = 8
const MAX_FONT_SIZE = 128
const DEFAULT_FONT_SIZE = 20
const EFFECT_CONTROLS = [
  { key: 'brightness', label: '字体亮度' },
  { key: 'glow', label: '荧光辉光 · Glow' },
  { key: 'bloom', label: '光晕扩散 · Bloom' },
  { key: 'flicker', label: '亮度闪烁 · Flicker' },
  { key: 'jitter', label: '文字抖动 · Jitter' },
  { key: 'horizontalSync', label: '水平同步干扰' },
  { key: 'rgbShift', label: 'RGB 色散' },
  { key: 'pixelGrid', label: '像素点阵' },
  { key: 'scanlines', label: '扫描线' },
  { key: 'vignette', label: '屏幕暗角' },
  { key: 'curvature', label: '边缘畸变' },
  { key: 'noise', label: '雪花质感' },
  { key: 'reflection', label: '边框反射' },
  { key: 'afterglow', label: '荧光残影' },
] as const

function loadFontSize(): number {
  try {
    const n: unknown = JSON.parse(localStorage.getItem('terminal-fontSize') ?? 'null')
    if (typeof n === 'number' && Number.isFinite(n) && n >= MIN_FONT_SIZE && n <= MAX_FONT_SIZE) return n
  } catch { /* ignore */ }
  return DEFAULT_FONT_SIZE
}

// Only detach the holder on unmount: the shell and its scrollback survive route changes.
function TerminalPane({ id, fontSize, appearance, index, canClose, onClose }: {
  id: string; fontSize: number; appearance: TerminalAppearance; index: number; canClose: boolean; onClose: () => void
}) {
  const bodyRef = useRef<HTMLDivElement>(null)
  const surfaceRef = useRef<HTMLDivElement>(null)
  const historyRef = useRef<HTMLCanvasElement>(null)
  const reflectionRef = useRef<HTMLCanvasElement>(null)
  const [status, setStatus] = useState('连接中')
  const [size, setSize] = useState('')

  useEffect(() => {
    const sess = getSession(id)
    const container = bodyRef.current
    if (!sess || !container) return
    let mounted = true
    const fit = () => {
      if (!mounted) return
      try { sess.fit.fit() } catch { /* ignore */ }
    }
    const updateSize = () => setSize(`${sess.term.cols} × ${sess.term.rows}`)
    const updateStatus = () => setStatus(
      sess.ws.readyState === WebSocket.OPEN ? '已连接'
        : sess.ws.readyState === WebSocket.CONNECTING ? '连接中' : '已断开',
    )
    const onError = () => setStatus('连接错误')
    sess.ws.addEventListener('open', updateStatus)
    sess.ws.addEventListener('close', updateStatus)
    sess.ws.addEventListener('error', onError)
    const resizeListener = sess.term.onResize(updateSize)

    container.appendChild(sess.holder)
    if (!sess.opened) {
      sess.term.open(sess.holder)
      sess.opened = true
    }
    sess.term.options.fontSize = fontSize
    fit()
    sess.term.focus()
    updateStatus()
    updateSize()
    if (sess.ws.readyState === WebSocket.OPEN) {
      sess.ws.send(`\x1b[resize:${sess.term.rows}x${sess.term.cols}`)
    }

    let rafId = 0
    const ro = new ResizeObserver(() => {
      cancelAnimationFrame(rafId)
      rafId = requestAnimationFrame(fit)
    })
    ro.observe(container)
    document.fonts?.ready.then(fit).catch(() => {})

    return () => {
      mounted = false
      cancelAnimationFrame(rafId)
      ro.disconnect()
      resizeListener.dispose()
      sess.ws.removeEventListener('open', updateStatus)
      sess.ws.removeEventListener('close', updateStatus)
      sess.ws.removeEventListener('error', onError)
      sess.holder.remove()
    }
  }, [id])

  useEffect(() => {
    const sess = getSession(id)
    if (!sess) return
    let mounted = true
    sess.term.options.fontSize = fontSize
    const fit = () => {
      if (!mounted) return
      try { sess.fit.fit(); sess.term.refresh(0, sess.term.rows - 1) } catch { /* detached */ }
      surfaceRef.current?.dispatchEvent(new Event(FONT_EVENT))
    }
    fit()
    loadTerminalFont(appearance.font, fontSize)
      .then(() => {
        if (!mounted) return
        sess.term.options.fontFamily = TERMINAL_FONTS[resolveTerminalFont(appearance.font)].family
        fit()
      }).catch(fit)
    return () => { mounted = false }
  }, [id, fontSize, appearance.font])

  useTerminalPhosphor(id, surfaceRef, historyRef, reflectionRef, appearance, fontSize)
  const curvature = appearance.enabled ? appearance.curvature : 0

  return (
    <section className="term-pane" data-terminal-id={id} aria-label={`终端 ${index + 1}`}>
      <div className="term-pane-bar">
        <span className="term-pane-title"><span className="term-pane-number">0{index + 1}</span> 终端 {index + 1}</span>
        <div className="term-pane-meta">
          <span className="term-pane-size" aria-label="终端列数与行数">{size}</span>
          <span className={`term-connection ${status === '已连接' ? 'is-connected' : ''}`} role="status">
            <span className="term-status-dot" />{status}
          </span>
          <button className="term-pane-close" aria-label={`关闭终端 ${index + 1}`}
            title={canClose ? '关闭此终端' : '至少保留一个终端'} disabled={!canClose} onClick={onClose}>×</button>
        </div>
      </div>
      <div className="term-screen">
        <div className="term-reflection-light" aria-hidden="true">
          <canvas className="term-frame-reflection" ref={reflectionRef} />
        </div>
        <TerminalCurvature filterId={`terminal-curvature-${id}`} surfaceRef={surfaceRef} strength={curvature}
          horizontalSync={appearance.enabled ? appearance.horizontalSync : 0} />
        <TerminalSignal id={`terminal-signal-${id}`} surfaceRef={surfaceRef}
          horizontalSync={appearance.enabled ? appearance.horizontalSync : 0} rgbShift={appearance.enabled ? appearance.rgbShift : 0}
          jitter={appearance.enabled ? appearance.jitter : 0} />
        <div className="term-crt-surface" ref={surfaceRef} style={{ margin: curvature > 0 ? SCREEN_RECESS : 0 }}>
          <div className="term-crt-emission">
            <div className="term-phosphor-light">
              <div className="term-phosphor-content">
                <div className="term-pane-body" ref={bodyRef} onMouseDown={() => getSession(id)?.term.focus()} />
                <canvas className="term-crt-afterglow" ref={historyRef} aria-hidden="true" />
              </div>
            </div>
            <div className="term-crt-overlay" aria-hidden="true">
              <div className="term-crt-scanlines" />
              <div className="term-crt-noise" />
              <div className="term-crt-pixels" />
              <div className="term-crt-glass" />
              <div className="term-crt-sweep" />
            </div>
          </div>
        </div>
      </div>
    </section>
  )
}

export default function TerminalPage() {
  const [fontSize, setFontSize] = useState<number>(loadFontSize)
  const [fontInput, setFontInput] = useState(() => String(loadFontSize()))
  const gridRef = useRef<HTMLDivElement>(null)
  const activeTerminalRef = useRef<string>()
  const [fullscreen, setFullscreen] = useState(false)
  const [fullscreenError, setFullscreenError] = useState('')
  const [ids, setIds] = useState<string[]>(listTerminals)
  const [appearance, setAppearance] = useState<TerminalAppearance>(loadTerminalAppearance)
  const [settingsOpen, setSettingsOpen] = useState(false)
  const profile = TERMINAL_PROFILES[appearance.profile]
  const selectedFont = TERMINAL_FONTS[resolveTerminalFont(appearance.font)]
  const fontLicense = selectedFont.license
  const idsRef = useRef(ids)
  idsRef.current = ids

  useEffect(() => {
    const grid = gridRef.current
    let focusFrame = 0
    const syncFullscreen = () => {
      setFullscreen(document.fullscreenElement === grid)
      cancelAnimationFrame(focusFrame)
      focusFrame = requestAnimationFrame(() => {
        const id = activeTerminalRef.current ?? idsRef.current[0]
        if (id) getSession(id)?.term.focus()
      })
    }
    const exitOnEscape = (event: KeyboardEvent) => {
      if (event.key !== 'Escape' || document.fullscreenElement !== grid) return
      event.preventDefault(); event.stopPropagation()
      void document.exitFullscreen().catch(() => {})
    }
    document.addEventListener('keydown', exitOnEscape, true)
    document.addEventListener('fullscreenchange', syncFullscreen)
    return () => {
      document.removeEventListener('fullscreenchange', syncFullscreen)
      document.removeEventListener('keydown', exitOnEscape, true)
      cancelAnimationFrame(focusFrame)
      if (document.fullscreenElement === grid) void document.exitFullscreen().catch(() => {})
    }
  }, [])

  const toggleFullscreen = async () => {
    setFullscreenError('')
    try {
      if (document.fullscreenElement === gridRef.current) await document.exitFullscreen()
      else await gridRef.current?.requestFullscreen()
    } catch {
      setFullscreenError('无法进入全屏，请允许浏览器全屏后重试。')
    }
  }
  const applyFontSize = (value: number) => {
    setFontSize(value)
    try { localStorage.setItem('terminal-fontSize', JSON.stringify(value)) } catch { /* ignore */ }
  }
  const commitFontSize = () => {
    const parsed = fontInput.trim() ? Number(fontInput) : fontSize
    const value = Number.isFinite(parsed) ? Math.min(MAX_FONT_SIZE, Math.max(MIN_FONT_SIZE, parsed)) : fontSize
    setFontInput(String(value)); applyFontSize(value)
  }

  useEffect(() => {
    if (listTerminals().length === 0) {
      openTerminal(loadFontSize())
      setIds(listTerminals())
    }
  }, [])

  useEffect(() => {
    saveTerminalAppearance(appearance)
  }, [appearance])

  useEffect(() => {
    const options = terminalAppearanceOptions(appearance)
    for (const id of ids) {
      const sess = getSession(id)
      if (sess) sess.term.options = options
    }
  }, [appearance.profile, appearance.enabled, ids])

  const updateAppearance = (change: Partial<TerminalAppearance>) => setAppearance(prev => ({ ...prev, ...change }))
  const addTerminal = () => {
    if (openTerminal(fontSize) !== null) setIds(listTerminals())
  }
  const removeTerminal = (id: string) => {
    closeTerminal(id)
    setIds(listTerminals())
  }

  const n = Math.max(ids.length, 1)
  const cols = Math.ceil(Math.sqrt(n))
  const rows = Math.ceil(n / cols)
  const halos = appearance.enabled ? terminalLightHalos(appearance.glow, appearance.bloom, profile.rgb) : []
  const screenStyle = {
    '--crt-foreground': profile.foreground,
    '--crt-text-brightness': appearance.enabled ? appearance.brightness / 100 : 1,
    '--crt-background': profile.background,
    '--crt-rgb': profile.rgb,
    '--crt-pixel-texture': terminalDotTexture(appearance.pixelSize),
    '--crt-pixel-size': `${appearance.pixelSize}px`,
    '--crt-pixel-grid': appearance.pixelGrid / 100,
    '--crt-scanlines': scanlineOpacity(appearance.scanlines),
    '--crt-vignette': vignetteOpacity(appearance.vignette),
    '--crt-vignette-start': `${VIGNETTE_START * 100}%`,
    '--crt-noise': appearance.noise > 0 ? .1 + appearance.noise / 100 * .4 : 0,
    '--crt-snow-texture': terminalSnowTexture(),
    '--crt-reflection': appearance.reflection / 100 * .85,
    '--crt-afterglow': afterglowOpacity(appearance.afterglow),
    '--crt-text-shadow': halos.map(h => `0 0 ${h.radius}px ${h.color}`).join(', ') || 'none',
    '--crt-flicker-low': 1 - appearance.flicker / 100 * .16,
    '--crt-flicker-mid': 1 - appearance.flicker / 100 * .07,
    '--crt-flicker-soft': 1 - appearance.flicker / 100 * .025,
    '--crt-jitter-x': `${appearance.jitter / 100 * .85}px`,
    '--crt-jitter-y': `${appearance.jitter / 100 * .35}px`,
  } as CSSProperties

  return (
    <div className={`terminal-page ${appearance.enabled ? 'crt-enabled' : ''} ${appearance.sweep ? 'crt-sweep-enabled' : ''} ${appearance.enabled && appearance.noise > 0 ? 'crt-snow-enabled' : ''} ${appearance.enabled && appearance.flicker > 0 ? 'crt-flicker-enabled' : ''} ${appearance.enabled && appearance.jitter > 0 ? 'crt-jitter-enabled' : ''}`} style={screenStyle}>
      <header className="terminal-header">
        <div className="terminal-heading">
          <span className="terminal-mark" aria-hidden="true">&gt;_</span>
          <div><h1>终端命令行</h1><span className="terminal-caption">{profile.caption}</span></div>
        </div>
        <div className="terminal-header-controls">
          <div className="terminal-profiles" role="group" aria-label="终端配色">
            {(Object.keys(TERMINAL_PROFILES) as TerminalProfile[]).map(key => (
              <button key={key} className="terminal-profile" aria-pressed={appearance.profile === key}
                onClick={() => updateAppearance({ profile: key, enabled: key !== 'modern' })}>
                <span className="terminal-swatch" style={{ background: TERMINAL_PROFILES[key].foreground }} />
                {TERMINAL_PROFILES[key].label}
              </button>
            ))}
          </div>
          <label className="terminal-font-control">字体
            <select className="terminal-select" aria-label="终端字体" value={resolveTerminalFont(appearance.font)}
              onChange={e => updateAppearance({ font: e.target.value as TerminalFont })}>
              {Object.entries(TERMINAL_FONTS).map(([key, font]) => <option key={key} value={key}>{font.label}</option>)}
            </select>
          </label>
          <label className="terminal-font-control">字号
            <input className="terminal-font-size" type="number" aria-label="字号" inputMode="decimal"
              title="字号单位为 CSS px；不同字体的字形高度会有差异。支持 8–128px。"
              min={MIN_FONT_SIZE} max={MAX_FONT_SIZE} step="0.5" value={fontInput}
              onChange={e => {
                const raw = e.target.value
                setFontInput(raw)
                const value = raw.trim() ? Number(raw) : NaN
                if (Number.isFinite(value) && value >= MIN_FONT_SIZE && value <= MAX_FONT_SIZE) applyFontSize(value)
              }} onBlur={commitFontSize}
              onKeyDown={e => {
                e.stopPropagation()
                if (e.key === 'Enter') { e.preventDefault(); e.currentTarget.blur() }
                if (e.key === 'Escape') { e.preventDefault(); setFontInput(String(fontSize)) }
              }} />
            <span>px</span>
          </label>
          <button className="terminal-control" onClick={toggleFullscreen} aria-pressed={fullscreen}
            title="全屏显示终端画面，按 Esc 退出">全屏</button>
          {fullscreenError && <span className="terminal-fullscreen-error" role="alert">{fullscreenError}</span>}
          <button className="terminal-control" aria-expanded={settingsOpen} aria-controls="terminal-display-settings"
            onClick={() => setSettingsOpen(open => !open)}>显示调校 <span aria-hidden="true">{settingsOpen ? '▴' : '▾'}</span></button>
          <button className="terminal-add" onClick={addTerminal} disabled={ids.length >= MAX_TERMINALS}>
            ＋ 新建分屏 <span>{ids.length}/{MAX_TERMINALS}</span>
          </button>
        </div>
      </header>

      {settingsOpen && (
        <div className="terminal-settings" id="terminal-display-settings" role="region" aria-label="显示调校">
          <label className="terminal-toggle"><input type="checkbox" checked={appearance.enabled}
            onChange={e => updateAppearance({ enabled: e.target.checked })} />CRT 效果</label>
          {EFFECT_CONTROLS.map(({ key, label }) => (
            <label className="terminal-effect-control" key={key} htmlFor={`terminal-effect-${key}`}>
              <span>{label}<output>{appearance[key]}%</output></span>
              <input id={`terminal-effect-${key}`} type="range" min="0" max={key === 'brightness' ? 200 : 100} step="1" value={appearance[key]} disabled={!appearance.enabled}
                onChange={e => updateAppearance({ [key]: +e.target.value })} />
            </label>
          ))}
          <label className="terminal-font-control">像素尺寸
            <select className="terminal-select" aria-label="像素尺寸" value={appearance.pixelSize} disabled={!appearance.enabled}
              onChange={e => updateAppearance({ pixelSize: +e.target.value })}>
              {[2, 3, 4, 5, 6].map(size => <option key={size} value={size}>{size}px</option>)}
            </select>
          </label>
          <label className="terminal-toggle"><input type="checkbox" checked={appearance.sweep} disabled={!appearance.enabled}
            onChange={e => updateAppearance({ sweep: e.target.checked })} />动态扫光</label>
          <button className="terminal-control" onClick={() => setAppearance({ ...DEFAULT_APPEARANCE })}>恢复默认</button>
        </div>
      )}

      <div className="terminal-grid" ref={gridRef} onFocusCapture={e => {
        const pane = (e.target as HTMLElement).closest<HTMLElement>('.term-pane')
        if (pane?.dataset.terminalId) activeTerminalRef.current = pane.dataset.terminalId
      }} style={{
        gridTemplateColumns: `repeat(${cols}, minmax(0, 1fr))`,
        gridTemplateRows: `repeat(${rows}, minmax(0, 1fr))`,
      }}>
        <div className="terminal-fullscreen-controls">
          <button className="terminal-control" onClick={toggleFullscreen} title="退出全屏（Esc）">退出全屏 · Esc</button>
        </div>
        {ids.map((id, i) => (
          <TerminalPane key={id} id={id} index={i} fontSize={fontSize}
            appearance={appearance}
            canClose={ids.length > 1} onClose={() => removeTerminal(id)} />
        ))}
      </div>
      <footer className="terminal-footer">
        <span><span className="terminal-footer-mode">{appearance.enabled ? 'CRT' : '清晰'}</span> {profile.label}</span>
        <span className="terminal-shortcuts">Ctrl+Shift+C 复制 · Ctrl+V 粘贴 · Ctrl+C 中断</span>
        {fontLicense && <a href={fontLicense} target="_blank" rel="noreferrer">{selectedFont.label} 字体许可</a>}
        <a href="https://github.com/Swordfish90/cool-retro-term" target="_blank" rel="noreferrer">灵感来自 cool-retro-term ↗</a>
      </footer>
    </div>
  )
}
