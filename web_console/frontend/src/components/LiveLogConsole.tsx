import { memo, useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react'
import { useAuthStore } from '../store/authStore'

const MAX_LINES = 1000
const MAX_PENDING = 5000
const FLUSH_MS = 100
const CONNECTION_TIMEOUT_MS = 45000

type LogLine = { id: number; text: string }

const Line = memo(function Line({ text }: { text: string }) {
  return <div className={`live-view-log-line${/ERROR|error|\[进程已停止\]/.test(text) ? ' error' : /WARN/.test(text) ? ' warning' : ''}`}>{text}</div>
})

// 日志更新独立于视频和控制面板；高频消息只入队，每 100ms 最多提交一次 DOM 更新。
export default memo(function LiveLogConsole({ appName }: { appName: string }) {
  const [lines, setLines] = useState<LogLine[]>([])
  const [connected, setConnected] = useState(false)
  const [following, setFollowing] = useState(true)
  const [pendingCount, setPendingCount] = useState(0)
  const boxRef = useRef<HTMLDivElement>(null)
  const followingRef = useRef(true)
  const pendingRef = useRef<LogLine[]>([])
  const sequenceRef = useRef(0)
  const lastScrollTopRef = useRef(0)

  const stickToBottom = useCallback(() => {
    const box = boxRef.current
    if (box && followingRef.current) {
      box.scrollTop = box.scrollHeight
      lastScrollTopRef.current = box.scrollTop
    }
  }, [])

  const flush = useCallback(() => {
    if (!followingRef.current) {
      setPendingCount(pendingRef.current.length)
      return
    }
    const added = pendingRef.current
    pendingRef.current = []
    setPendingCount(0)
    if (added.length) setLines(previous => [...previous, ...added].slice(-MAX_LINES))
  }, [])

  const resume = () => {
    followingRef.current = true
    setFollowing(true)
    flush()
    stickToBottom()
  }

  useLayoutEffect(stickToBottom, [lines, stickToBottom])

  useEffect(() => {
    const box = boxRef.current
    if (!box) return
    const observer = new ResizeObserver(stickToBottom)
    observer.observe(box)
    return () => observer.disconnect()
  }, [stickToBottom])

  useEffect(() => {
    let disposed = false
    let socket: WebSocket | null = null
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null
    let flushTimer: ReturnType<typeof setTimeout> | null = null
    let lastMessageAt = performance.now()
    followingRef.current = true
    pendingRef.current = []
    setLines([])
    setPendingCount(0)
    setFollowing(true)
    setConnected(false)

    const scheduleFlush = () => {
      if (flushTimer !== null) return
      flushTimer = setTimeout(() => {
        flushTimer = null
        if (!disposed) flush()
      }, FLUSH_MS)
    }

    const connect = () => {
      if (disposed || !appName) return
      const token = useAuthStore.getState().token ?? ''
      const protocol = location.protocol === 'https:' ? 'wss' : 'ws'
      const ws = new WebSocket(`${protocol}://${location.host}/ws/logs/${encodeURIComponent(appName)}?token=${encodeURIComponent(token)}`)
      socket = ws
      lastMessageAt = performance.now()
      ws.onopen = () => { if (!disposed) setConnected(true) }
      ws.onmessage = event => {
        if (disposed || socket !== ws) return
        lastMessageAt = performance.now() // 空帧也是服务端心跳。
        const added = String(event.data).split('\n').filter(Boolean).slice(-MAX_PENDING)
        for (const text of added) pendingRef.current.push({ id: sequenceRef.current++, text: text.slice(0, 2048) })
        if (pendingRef.current.length > MAX_PENDING) pendingRef.current.splice(0, pendingRef.current.length - MAX_PENDING)
        if (added.length) scheduleFlush()
      }
      ws.onclose = () => {
        if (disposed || socket !== ws) return
        setConnected(false)
        reconnectTimer = setTimeout(connect, 1500)
      }
      ws.onerror = () => ws.close()
    }
    connect()

    // 检测没有 close 事件的半开连接；单纯没有业务日志时由后端心跳保活。
    const checkConnection = () => {
      if (document.hidden) return
      if (socket && (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) &&
          performance.now() - lastMessageAt > CONNECTION_TIMEOUT_MS) socket.close()
      flush()
      stickToBottom()
    }
    const watchdog = setInterval(checkConnection, 5000)
    const onVisible = () => { if (!document.hidden) checkConnection() }
    document.addEventListener('visibilitychange', onVisible)
    return () => {
      disposed = true
      if (reconnectTimer !== null) clearTimeout(reconnectTimer)
      if (flushTimer !== null) clearTimeout(flushTimer)
      clearInterval(watchdog)
      document.removeEventListener('visibilitychange', onVisible)
      socket?.close()
      pendingRef.current = []
    }
  }, [appName, flush, stickToBottom])

  const onScroll = () => {
    const box = boxRef.current
    if (!box) return
    const nearBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 40
    if (nearBottom && !followingRef.current) resume()
    else if (!nearBottom && box.scrollTop < lastScrollTopRef.current - 1) {
      // 只有向上滚动才暂停；容器缩放、裁剪旧行等布局变化不切换跟随状态。
      followingRef.current = false
      setFollowing(false)
    }
    lastScrollTopRef.current = box.scrollTop
  }

  return <section className="live-view-console-panel">
    <div className="live-view-panel-head">
      <span title="保留最近 1000 行；向上滚动暂停跟随，点击“跳到底部”恢复。">终端输出</span>
      <div className="live-view-console-tools">
        <span className={`live-view-log-status ${connected ? following ? 'online' : 'paused' : ''}`}>
          {connected ? following ? '● 实时' : 'Ⅱ 已暂停跟随' : '○ 重连中'}
        </span>
        <button onClick={resume}>跳到底部{pendingCount > 0 ? `（${pendingCount >= MAX_PENDING ? '5000+' : pendingCount}）` : ''}</button>
      </div>
    </div>
    <div className="live-view-console" ref={boxRef} onScroll={onScroll}>
      {lines.length === 0 ? <div className="live-view-console-empty">暂无终端输出。</div>
        : lines.map(line => <Line key={line.id} text={line.text} />)}
    </div>
  </section>
})
