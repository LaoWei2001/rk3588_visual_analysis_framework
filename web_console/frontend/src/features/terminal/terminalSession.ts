import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { useAuthStore } from '../../store/authStore'
import { apiCheckSession } from '../../api/auth'
import { registerTerminalCleanup } from './sessionLifecycle'
import { loadTerminalAppearance, terminalAppearanceOptions, TERMINAL_FONTS } from './terminalAppearance'
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
  status: string
  events: EventTarget
  disconnect: () => void
  holder: HTMLDivElement
  opened: boolean   // term.open() 只能调一次，且必须在 holder 进入 DOM 后
}

export const MAX_TERMINALS = 4

const sessions = new Map<string, TermSession>()
let order: string[] = []   // 显示顺序
let seq = 0

function createSession(id: string, fontSize: number): TermSession {
  const term = new Terminal({
    fontSize,
    fontFamily: TERMINAL_FONTS.modern.family,
    ...terminalAppearanceOptions(loadTerminalAppearance()),
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
  const nonce = crypto.getRandomValues(new Uint8Array(16))
  const sessionId = Array.from(nonce, byte => byte.toString(16).padStart(2, '0')).join('')
  const events = new EventTarget()
  let ws: WebSocket
  let status = '连接中'
  let disposed = false, ended = false, negotiated = false, resumable = false, openedOnce = false
  let receivedOffset = 0, appliedOffset = 0, lastSeen = Date.now(), retries = 0
  let retryTimer: ReturnType<typeof setTimeout> | undefined
  let ackTimer: ReturnType<typeof setTimeout> | undefined
  let connectTimer: ReturnType<typeof setTimeout> | undefined
  let heartbeatTimer: ReturnType<typeof setInterval> | undefined
  const setStatus = (value: string) => {
    status = value
    events.dispatchEvent(new Event('status'))
  }
  const sendControl = (type: string) => {
    if (negotiated && ws.readyState === WebSocket.OPEN) {
      ws.send(new TextEncoder().encode(JSON.stringify({ type, offset: appliedOffset })))
    }
  }
  const sendSize = () => {
    if (ws.readyState === WebSocket.OPEN) ws.send('\x1b[resize:' + term.rows + 'x' + term.cols)
  }
  term.onResize(() => sendSize())
  const clearTransportTimers = () => {
    clearTimeout(ackTimer); clearTimeout(connectTimer); clearInterval(heartbeatTimer)
    ackTimer = undefined; connectTimer = undefined; heartbeatTimer = undefined
  }
  function scheduleRetry() {
    if (disposed || ended || useAuthStore.getState().token !== token) return
    clearTimeout(retryTimer)
    if (!navigator.onLine) { setStatus('等待网络'); return }
    setStatus('正在重连')
    const delay = Math.min(15000, 500 * 2 ** Math.min(retries++, 5)) * (.8 + Math.random() * .4)
    retryTimer = setTimeout(connect, delay)
  }
  function recover(force = false) {
    if (disposed || ended || document.hidden || useAuthStore.getState().token !== token) return
    if (ws.readyState === WebSocket.OPEN && !force) return
    if (ws.readyState === WebSocket.CONNECTING && !force) return
    clearTimeout(retryTimer)
    const old = ws
    connect()
    try { old.close() } catch { /* transport may already be gone */ }
  }
  function connect() {
    if (disposed || ended || useAuthStore.getState().token !== token) return
    clearTransportTimers()
    negotiated = false
    const params = new URLSearchParams({ token, protocol: '2', sid: sessionId,
      offset: String(appliedOffset), resume: resumable ? '1' : '0' })
    const socket = new WebSocket((location.protocol === 'https:' ? 'wss' : 'ws')
      + '://' + location.host + '/ws/terminal?' + params)
    ws = socket
    ws.binaryType = 'arraybuffer'
    setStatus(openedOnce ? '正在重连' : '连接中')
    connectTimer = setTimeout(() => {
      if (ws === socket && (socket.readyState === WebSocket.CONNECTING || (resumable && !negotiated))) recover(true)
    }, 15000)
    socket.onopen = () => {
      if (ws !== socket || disposed) return
      openedOnce = true
      lastSeen = Date.now()
      if (!resumable) {
        setStatus('已连接') // compatibility with a server deployed before protocol 2
        term.options.disableStdin = false
        if (holder.isConnected && !document.hidden) term.focus()
      }
      sendSize()
    }
    socket.onmessage = e => {
      if (ws !== socket || disposed) return
      lastSeen = Date.now()
      if (typeof e.data === 'string') {
        let message: { type?: string; exit_code?: number | null } | undefined
        try { message = JSON.parse(e.data) } catch { /* legacy terminal text */ }
        if (message?.type === 'terminal.ready') {
          negotiated = resumable = true
          retries = 0
          clearTimeout(connectTimer)
          term.options.disableStdin = false
          setStatus('已连接')
          sendSize(); sendControl('ack')
          clearInterval(heartbeatTimer)
          heartbeatTimer = setInterval(() => {
            if (Date.now() - lastSeen > 60000 && !document.hidden) recover(true)
            else sendControl('pong')
          }, 15000)
          return
        }
        if (negotiated && message?.type === 'terminal.ping') { sendControl('pong'); return }
        if (negotiated && message?.type === 'terminal.exit') {
          ended = true
          term.options.disableStdin = true
          setStatus('进程已退出')
          term.writeln('\r\n\x1b[33m[终端进程已退出]\x1b[0m')
          return
        }
        term.write(e.data)
      } else if (e.data instanceof ArrayBuffer) {
        if (!negotiated) { term.write(new Uint8Array(e.data)); return }
        if (e.data.byteLength < 8) { socket.close(1002, 'invalid terminal frame'); return }
        const header = new DataView(e.data)
        const end = header.getUint32(0) * 4294967296 + header.getUint32(4)
        const data = new Uint8Array(e.data, 8), start = end - data.length
        if (!Number.isSafeInteger(end) || start < 0 || start > receivedOffset) {
          socket.close(1002, 'terminal output gap'); return
        }
        if (end <= receivedOffset) { sendControl('ack'); return }
        const unseen = data.subarray(Math.max(0, receivedOffset - start))
        receivedOffset = end
        term.write(unseen, () => {
          appliedOffset = Math.max(appliedOffset, end)
          if (disposed) return
          if (ackTimer === undefined) ackTimer = setTimeout(() => {
            ackTimer = undefined; sendControl('ack')
          }, 32)
        })
      }
    }
    socket.onclose = e => {
      if (ws !== socket || disposed) return
      clearTransportTimers()
      term.options.disableStdin = true
      if (useAuthStore.getState().token === token) void apiCheckSession().catch(() => {})
      if (ended) return
      if ([4004, 4001, 1002, 1008, 1009].includes(e.code)) {
        ended = true
        setStatus('会话已结束')
        term.writeln('\r\n\x1b[33m[终端会话已结束，请新建终端]\x1b[0m')
      } else if (resumable || !openedOnce) scheduleRetry()
      else {
        ended = true
        setStatus('已断开')
        term.writeln('\r\n\x1b[31m[连接已断开，代码 ' + e.code + ']\x1b[0m')
      }
    }
    socket.onerror = () => {
      if (ws === socket && !disposed && !ended) setStatus(resumable ? '正在重连' : '连接错误')
    }
  }
  const online = () => recover()
  const visible = () => {
    if (!document.hidden) recover(resumable && Date.now() - lastSeen > 60000)
  }
  window.addEventListener('online', online)
  document.addEventListener('visibilitychange', visible)
  function disconnect() {
    if (disposed) return
    disposed = true
    clearTimeout(retryTimer); clearTransportTimers()
    window.removeEventListener('online', online)
    document.removeEventListener('visibilitychange', visible)
    window.removeEventListener('pagehide', pagehide)
    sendControl('close')
    ws.onclose = ws.onerror = ws.onmessage = ws.onopen = null
    try { ws.close() } catch { /* already closed */ }
  }
  function pagehide(event: PageTransitionEvent) { if (!event.persisted) disconnect() }
  window.addEventListener('pagehide', pagehide)
  connect()
  term.onData(data => {
    if (ws.readyState === WebSocket.OPEN && (!resumable || negotiated)) ws.send(data)
  })
  return { id, term, fit, get ws() { return ws }, holder, opened: false,
    events, get status() { return status }, disconnect }
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
  sess.disconnect()
  try { sess.term.dispose() } catch { /* ignore */ }
  sess.holder.remove()
  sessions.delete(id)
  order = order.filter(x => x !== id)
}

/** 退出登录/整体结束：关闭并回收所有终端。 */
export function destroyAllTerminals(): void {
  for (const id of [...order]) closeTerminal(id)
}

registerTerminalCleanup(destroyAllTerminals)
