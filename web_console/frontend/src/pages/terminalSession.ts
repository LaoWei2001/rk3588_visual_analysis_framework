import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { useAuthStore } from '../store/authStore'
import '@xterm/xterm/css/xterm.css'

// 终端会话注册表：可同时存在多个终端，每个对应一条独立 WebSocket → 板端一个独立 shell。
// 这些会话在模块级常驻，不随路由切换卸载 —— 切到别的页面再回来，所有分屏和会话都还在。
// 整页刷新会把本模块连同所有 WebSocket 一起销毁 → 后端 PTY 随之回收 → 回到单个新终端。
// 关闭某个分屏 / 退出登录时显式断连，板端 shell 立即回收。

// 复制文本：优先用 Clipboard API(需 HTTPS/localhost)，HTTP 下回退到 execCommand('copy')。
function copyText(text: string): void {
  if (!text) return
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text).catch(() => fallbackCopy(text))
  } else {
    fallbackCopy(text)
  }
}
function fallbackCopy(text: string): void {
  try {
    const ta = document.createElement('textarea')
    ta.value = text
    ta.style.position = 'fixed'
    ta.style.left = '-9999px'
    ta.setAttribute('readonly', '')
    document.body.appendChild(ta)
    ta.select()
    document.execCommand('copy')
    document.body.removeChild(ta)
  } catch { /* ignore */ }
}

export interface TermSession {
  id: string
  term: Terminal
  fit: FitAddon
  ws: WebSocket
  holder: HTMLDivElement
  opened: boolean   // term.open() 只能调一次，且必须在 holder 进入 DOM 后
}

export const MAX_TERMINALS = 4

const sessions = new Map<string, TermSession>()
let order: string[] = []   // 显示顺序
let seq = 0

function createSession(id: string, fontSize: number): TermSession {
  const term = new Terminal({
    cursorBlink: true,
    cursorStyle: 'bar',
    fontSize,
    fontFamily: 'Consolas,monospace',
    theme: {
      background: '#080b14',
      foreground: '#cbd5e1',
      cursor: '#cbd5e1',
      selectionBackground: '#2e3352',
      black:   '#1a1d29',
      red:     '#f87171',
      green:   '#4ade80',
      yellow:  '#fbbf24',
      blue:    '#60a5fa',
      magenta: '#c084fc',
      cyan:    '#22d3ee',
      white:   '#e6e9ef',
      brightBlack:   '#4b5563',
      brightRed:     '#fca5a5',
      brightGreen:   '#86efac',
      brightYellow:  '#fde68a',
      brightBlue:    '#93c5fd',
      brightMagenta: '#d8b4fe',
      brightCyan:    '#67e8f9',
      brightWhite:   '#f9fafb',
    },
  })

  // @xterm/xterm@6.0.0 的压缩产物在处理 DECRQM「请求模式」查询时会抛
  // `ReferenceError: r is not defined`(requestMode 内),异常发生在 _innerWrite 里，
  // 直接打断写入/渲染管线 —— 表现为：开场白画出来后，之后所有输出(打字/退出)都不再刷新。
  // vim 启动会发 ESC[?12$p 这类 DECRQM，正好踩中。这里抢先拦截 DECRQM(CSI $p / CSI ?$p)，
  // 返回 true 表示已处理，内置那个会崩的 requestMode 就不会被调用。vim 不依赖它的回应。
  term.parser.registerCsiHandler({ intermediates: '$', final: 'p' }, () => true)
  term.parser.registerCsiHandler({ prefix: '?', intermediates: '$', final: 'p' }, () => true)

  const fit = new FitAddon()
  term.loadAddon(fit)

  // 常驻容器：挂到哪个分屏就把它整体 append 过去；term.open() 推迟到它进入 DOM 后再调
  // （在脱离 DOM 的元素上 open 会把字符尺寸量成 0，导致行列计算错乱）。
  const holder = document.createElement('div')
  holder.style.width = '100%'
  holder.style.height = '100%'

  // ── 复制 / 粘贴 / 输入法误发 ^C 处理 ───────────────────────────────
  const copySelection = () => {
    const sel = term.getSelection()
    if (sel) copyText(sel)
  }
  const pasteClipboard = () => {
    // 读剪贴板需安全上下文(HTTPS/localhost)；HTTP 下读不了，靠原生 Ctrl+V(xterm 的 paste 事件)粘贴。
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.readText().then(t => { if (t) term.paste(t) }).catch(() => { /* ignore */ })
    }
  }
  // 记录最近一次鼠标交互，用来识别"输入法划词/选中即复制"合成的 Ctrl+C
  let lastMouseTs = 0
  const markMouse = () => { lastMouseTs = Date.now() }
  holder.addEventListener('mousedown', markMouse, true)
  holder.addEventListener('mouseup', markMouse, true)

  // 搜狗等中文输入法的"划词/选中即复制"会在双击选词、拖选松手、甚至点击时合成一次 Ctrl+C，
  // 被 xterm 当成 SIGINT 发出 \x03(屏幕上就是 ^C)。这里挡掉它，并提供顺手的复制/粘贴快捷键：
  //   Ctrl+C：有选区 / 合成事件(!isTrusted) / 刚有鼠标交互 → 复制并拦截；否则(纯键盘无选区) → 照常 SIGINT。
  //   Ctrl+Shift+C 复制；Ctrl+Shift+V、Shift+Insert 粘贴(原生 Ctrl+V 也能粘)。
  term.attachCustomKeyEventHandler((e) => {
    if (e.type !== 'keydown') return true
    const isC = e.key === 'c' || e.key === 'C' || e.code === 'KeyC' || e.keyCode === 67
    const isV = e.key === 'v' || e.key === 'V' || e.code === 'KeyV' || e.keyCode === 86
    const isD = e.key === 'd' || e.key === 'D' || e.code === 'KeyD' || e.keyCode === 68
    // Ctrl+D 会向 shell 发 EOF(\x04)，在空命令行上等于退出/注销当前 shell —— 用户不需要该功能，直接挡掉。
    if (isD && e.ctrlKey && !e.shiftKey && !e.altKey && !e.metaKey) return false
    if (isC && e.ctrlKey && !e.altKey && !e.metaKey && !e.shiftKey) {
      if (term.hasSelection() || !e.isTrusted || Date.now() - lastMouseTs < 250) {
        copySelection()
        return false   // 挡掉 ^C
      }
      return true      // 纯键盘、无选区 → 作为 SIGINT
    }
    // 普通 Ctrl+V：放行(return false)给浏览器原生粘贴 —— xterm 自己监听 paste 事件读 clipboardData，
    // HTTP 下也能用。若不放行，xterm 默认会把 Ctrl+V 当控制字符 \x16 发出，表现就是"按了没反应"。
    if (isV && e.ctrlKey && !e.shiftKey && !e.altKey && !e.metaKey) return false
    if (isC && e.ctrlKey && e.shiftKey && !e.altKey && !e.metaKey) { copySelection(); return false }
    if (isV && e.ctrlKey && e.shiftKey && !e.altKey && !e.metaKey) { pasteClipboard(); return false }
    if (e.shiftKey && (e.key === 'Insert' || e.code === 'Insert')) { pasteClipboard(); return false }
    return true
  })

  // 右键：有选区→复制，无选区→粘贴(PuTTY/Windows 终端风格)，并屏蔽浏览器右键菜单
  holder.addEventListener('contextmenu', (e) => {
    e.preventDefault()
    if (term.hasSelection()) copySelection()
    else pasteClipboard()
  })

  const token = useAuthStore.getState().token ?? ''
  const wsUrl = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws/terminal?token=${encodeURIComponent(token)}`
  const ws = new WebSocket(wsUrl)
  ws.binaryType = 'arraybuffer'

  const sendSize = () => {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(`\x1b[resize:${term.rows}x${term.cols}`)
    }
  }
  // 行列变化(fit 重排)就把真实尺寸同步给 PTY，避免 vim 等按错误行数渲染。
  term.onResize(() => sendSize())

  ws.onopen = () => {
    // 此时 holder 已被分屏组件同步 append 进容器并 fit 过，term.rows/cols 已是真实值。
    sendSize()
    term.focus()
  }
  ws.onmessage = (e) => {
    if (e.data instanceof ArrayBuffer) {
      term.write(new Uint8Array(e.data))
    } else if (typeof e.data === 'string') {
      term.write(e.data)
    }
  }
  ws.onclose = () => term.writeln('\r\n\x1b[31m[连接已断开]\x1b[0m')
  ws.onerror = () => term.writeln('\r\n\x1b[31m[连接错误]\x1b[0m')

  term.onData(data => {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(data)
    }
  })

  return { id, term, fit, ws, holder, opened: false }
}

/** 当前所有终端 id（显示顺序）。 */
export function listTerminals(): string[] {
  return [...order]
}

export function getSession(id: string): TermSession | undefined {
  return sessions.get(id)
}

/** 新建一个终端会话，返回其 id。达到上限时返回 null。 */
export function openTerminal(fontSize: number): string | null {
  if (order.length >= MAX_TERMINALS) return null
  const id = `t${++seq}`
  sessions.set(id, createSession(id, fontSize))
  order.push(id)
  return id
}

/** 关闭某个终端：断开 WebSocket(板端回收 shell)、销毁终端、移出注册表。 */
export function closeTerminal(id: string): void {
  const sess = sessions.get(id)
  if (!sess) return
  try { sess.ws.close() } catch { /* ignore */ }
  try { sess.term.dispose() } catch { /* ignore */ }
  sess.holder.remove()
  sessions.delete(id)
  order = order.filter(x => x !== id)
}

/** 退出登录/整体结束：关闭并回收所有终端。 */
export function destroyAllTerminals(): void {
  for (const id of [...order]) closeTerminal(id)
}
