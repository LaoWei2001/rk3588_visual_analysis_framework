import { useEffect, useRef, useState } from 'react'
import { useParams, useNavigate } from 'react-router-dom'
import { fetchLogTail } from '../api/client'
import { useAuthStore } from '../store/authStore'
import './LogsPage.css'

export default function LogsPage() {
  const { appName } = useParams()
  const navigate = useNavigate()
  const [lines, setLines] = useState<string[]>([])
  const [connected, setConnected] = useState(false)
  const [autoScroll, setAutoScroll] = useState(true)
  const containerRef = useRef<HTMLDivElement>(null)
  const wsRef = useRef<WebSocket | null>(null)
  const autoScrollRef = useRef(true)

  useEffect(() => { autoScrollRef.current = autoScroll }, [autoScroll])

  const scrollToBottom = () => {
    const el = containerRef.current
    if (el && autoScrollRef.current) el.scrollTop = el.scrollHeight
  }

  useEffect(() => {
    if (!appName) return

    // Load tail first
    fetchLogTail(appName, 300).then(data => {
      setLines(Array.isArray(data.lines) ? data.lines : [])
      setTimeout(scrollToBottom, 50)
    }).catch(() => {})

    // Connect WebSocket — pass auth token as query param
    const token = useAuthStore.getState().token ?? ''
    const wsUrl = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws/logs/${appName}?token=${encodeURIComponent(token)}`
    const ws = new WebSocket(wsUrl)
    wsRef.current = ws

    ws.onopen = () => setConnected(true)
    ws.onclose = () => setConnected(false)
    ws.onmessage = (e) => {
      const text = String(e.data)
      if (!text) return  // keepalive empty frame
      const newLines = text.split('\n').filter(l => l !== '')
      // 跟随到底时维持 2000 行上限；用户拉上去看历史时不裁顶部(放宽到 5000)——
      // 否则裁掉顶部旧行会把视口内容往上挤，导致历史信息闪烁、看不清。
      setLines(prev => {
        const next = [...prev, ...newLines]
        return autoScrollRef.current ? next.slice(-2000) : next.slice(-5000)
      })
      setTimeout(scrollToBottom, 10)
    }

    return () => { ws.close(); wsRef.current = null }
  }, [appName])

  const handleScroll = () => {
    const el = containerRef.current
    if (!el) return
    // 在底部(40px 内)=跟随；往上拉=暂停并停在当前位置；拉回底部=自动恢复跟随
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 40
    autoScrollRef.current = atBottom                              // 立即生效，供下一条日志的 scrollToBottom 判定
    setAutoScroll(prev => (prev === atBottom ? prev : atBottom))  // 同步「自动滚动」复选框显示
  }

  const handleClear = () => setLines([])

  return (
    <div className="logs-page">
      <div className="logs-header">
        <button className="tb-btn" onClick={() => navigate('/')}>← 返回</button>
        <span className="logs-title">{appName} — 运行日志</span>
        <div className={`ws-status ${connected ? 'on' : 'off'}`}>
          {connected ? '● 实时' : '○ 断开'}
        </div>
        <label className="autoscroll-toggle">
          <input type="checkbox" checked={autoScroll} onChange={e => setAutoScroll(e.target.checked)} />
          自动滚动
        </label>
        <button className="tb-btn" onClick={handleClear}>清空</button>
        <button className="tb-btn" onClick={() => {
          setAutoScroll(true)
          setTimeout(scrollToBottom, 10)
        }}>跳到底部</button>
      </div>

      <div className="logs-container" ref={containerRef} onScroll={handleScroll}>
        {lines.length === 0 ? (
          <div className="logs-empty">暂无日志…</div>
        ) : (
          lines.map((line, i) => (
            <div key={i} className={`log-line${line.includes('[进程已停止]') || line.includes('ERROR') || line.includes('error') ? ' err' : line.includes('WARN') ? ' warn' : ''}`}>
              {line}
            </div>
          ))
        )}
      </div>
    </div>
  )
}
