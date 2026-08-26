import { useEffect, useState, useRef } from 'react'
import { BrowserRouter, Routes, Route, NavLink, Navigate, useNavigate } from 'react-router-dom'
import AppsPage    from './pages/AppsPage'
import LiveViewPage from './pages/LiveViewPage'
import VideoCapturePage from './pages/VideoCapturePage'
import EditorPage  from './pages/EditorPage'
import LogsPage    from './pages/LogsPage'
import RecordsPage from './pages/RecordsPage'
import LoginPage   from './pages/LoginPage'
import TerminalPage from './pages/TerminalPage'
import ServicesPage from './pages/ServicesPage'
import SystemSettingsPage from './pages/SystemSettingsPage'
import CameraSettingsPage from './pages/CameraSettingsPage'
import { destroyAllTerminals } from './pages/terminalSession'
import ErrorBoundary from './components/ErrorBoundary'
import { useAuthStore } from './store/authStore'
import { useEditorStore } from './store/editorStore'
import { apiLogout }    from './api/client'
import './App.css'

// 编辑器或当前应用集成有未保存改动时，侧边栏跳转/退出前先确认。
const confirmLeaveIfDirty = (): boolean => {
  const state = useEditorStore.getState()
  if (state.dirty) {
    return window.confirm('编辑器有未保存的改动，确定离开？未保存的修改将丢失。')
  }
  if (state.appIntegrationDirty) {
    return window.confirm('当前程序的应用集成有未保存改动，确定离开？')
  }
  return true
}

// ── Sidebar logo: image on top, text below; no fallback if image missing ──
// 每次打开随机取一张(frontend/logos/ 里的图片或 GIF; 空则回退 logo.png)。
// ?t=<nonce> 让每次进入都重新向后端请求 → 重新随机；GIF 由 <img> 原生播放。
function SidebarLogo() {
  const [failed, setFailed] = useState(false)
  const src = useRef(`/logo/random?t=${Date.now()}`).current
  return (
    <div className="logo">
      {!failed && (
        <img src={src} className="sidebar-logo-img" alt=""
             onError={() => setFailed(true)} />
      )}
      <span className="logo-text">RK3588 控制台</span>
    </div>
  )
}

// ── Protected route wrapper ───────────────────────────────────────────────
function ProtectedRoute({ children }: { children: React.ReactNode }) {
  const token = useAuthStore(s => s.token)
  if (!token) return <Navigate to="/login" replace />
  return <>{children}</>
}

// ── Main shell (sidebar + content) ────────────────────────────────────────
function AppShell() {
  const navigate  = useNavigate()
  const username  = useAuthStore(s => s.username)
  const clearAuth = useAuthStore(s => s.clearAuth)

  const handleLogout = async () => {
    if (!confirmLeaveIfDirty()) return
    destroyAllTerminals()  // 主动断开所有终端 WebSocket，让板端回收 shell
    try { await apiLogout() } catch { /* token already expired is fine */ }
    clearAuth()
    navigate('/login', { replace: true })
  }

  // 取消则阻止 NavLink 跳转（preventDefault 后 React Router 不再导航）
  const guardNav = (e: React.MouseEvent) => { if (!confirmLeaveIfDirty()) e.preventDefault() }

  return (
    <div className="app-shell">
      <nav className="sidebar">
        <SidebarLogo />

        <NavLink to="/" end onClick={guardNav} className={({ isActive }) => isActive ? 'nav-item active' : 'nav-item'}>
          <span className="nav-icon">▣</span> 程序管理
        </NavLink>

        <NavLink to="/live-view" onClick={guardNav} className={({ isActive }) => isActive ? 'nav-item active' : 'nav-item'}>
          <span className="nav-icon">▰</span> 实时画面
        </NavLink>

        <NavLink to="/video-capture" onClick={guardNav} className={({ isActive }) => isActive ? 'nav-item active' : 'nav-item'}>
          <span className="nav-icon">●</span> 视频采集
        </NavLink>

        <NavLink to="/services" onClick={guardNav} className={({ isActive }) => isActive ? 'nav-item active' : 'nav-item'}>
          <span className="nav-icon">⚙</span> 系统服务
        </NavLink>

        <NavLink to="/system-settings" onClick={guardNav} className={({ isActive }) => isActive ? 'nav-item active' : 'nav-item'}>
          <span className="nav-icon">◉</span> 系统设置
        </NavLink>

        <NavLink to="/camera-settings" onClick={guardNav} className={({ isActive }) => isActive ? 'nav-item active' : 'nav-item'}>
          <span className="nav-icon">◈</span> 摄像头配置
        </NavLink>

        <NavLink to="/terminal" onClick={guardNav} className={({ isActive }) => isActive ? 'nav-item active' : 'nav-item'}>
          <span className="nav-icon">▤</span> 终端命令行
        </NavLink>

        {/* Bottom: user info + logout */}
        <div className="sidebar-footer">
          <span className="sidebar-user">👤 {username}</span>
          <button className="sidebar-logout" onClick={handleLogout}>退出登录</button>
        </div>
      </nav>

      <main className="main-content">
        <ErrorBoundary>
          <Routes>
            <Route path="/"                  element={<AppsPage />} />
            <Route path="/live-view"         element={<LiveViewPage />} />
            <Route path="/video-capture"     element={<VideoCapturePage />} />
            <Route path="/editor/:appName"   element={<EditorPage />} />
            <Route path="/logs/:appName"     element={<LogsPage />} />
            <Route path="/records/:appName"  element={<RecordsPage />} />
            <Route path="/terminal"        element={<TerminalPage />} />
            <Route path="/services"        element={<ServicesPage />} />
            <Route path="/system-settings" element={<SystemSettingsPage />} />
            <Route path="/camera-settings" element={<CameraSettingsPage />} />
          </Routes>
        </ErrorBoundary>
      </main>
    </div>
  )
}

// ── Root ──────────────────────────────────────────────────────────────────
export default function App() {
  useEffect(() => {
    const normalTitle = document.title
    let restoreTimer: number | undefined

    const clearTimers = () => {
      if (restoreTimer !== undefined) window.clearTimeout(restoreTimer)
      restoreTimer = undefined
    }

    const handleVisibilityChange = () => {
      clearTimers()

      if (document.hidden) {
        document.title = 'Σ(ﾟДﾟ；) 干嘛去了?'
        restoreTimer = window.setTimeout(() => {
          if (document.hidden) document.title = normalTitle
        }, 1000)
        return
      }

      document.title = '(｡•́‿•̀｡) 欢迎回来!'
      restoreTimer = window.setTimeout(() => {
        if (!document.hidden) document.title = normalTitle
      }, 1000)
    }

    document.addEventListener('visibilitychange', handleVisibilityChange)
    return () => {
      clearTimers()
      document.removeEventListener('visibilitychange', handleVisibilityChange)
      document.title = normalTitle
    }
  }, [])

  return (
    <BrowserRouter>
      <Routes>
        <Route path="/login" element={<LoginPage />} />
        <Route path="/*" element={
          <ProtectedRoute>
            <AppShell />
          </ProtectedRoute>
        } />
      </Routes>
    </BrowserRouter>
  )
}
