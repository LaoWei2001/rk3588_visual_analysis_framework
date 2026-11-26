import { lazy, Suspense, useEffect, useState, useRef } from 'react'
import { BrowserRouter, Routes, Route, NavLink, Navigate, useNavigate } from 'react-router-dom'
import AppsPage    from './pages/AppsPage'
import LoginPage   from './pages/LoginPage'
import { destroyAllTerminals } from './features/terminal/sessionLifecycle'
import ErrorBoundary from './components/ErrorBoundary'
import { useAuthStore } from './store/authStore'
import { useEditorStore } from './store/editorStore'
import { apiLogout, apiCheckSession } from './api/client'
import './App.css'

const LiveViewPage = lazy(() => import('./pages/LiveViewPage'))
const VideoCapturePage = lazy(() => import('./pages/VideoCapturePage'))
const DatasetPage = lazy(() => import('./pages/DatasetPage'))
const EditorPage = lazy(() => import('./pages/EditorPage'))
const LogsPage = lazy(() => import('./pages/LogsPage'))
const RecordsPage = lazy(() => import('./pages/RecordsPage'))
const ImageInferencePage = lazy(() => import('./features/image-inference/ImageInferencePage'))
const TerminalPage = lazy(() => import('./features/terminal/TerminalPage'))
const ServicesPage = lazy(() => import('./pages/ServicesPage'))
const SystemSettingsPage = lazy(() => import('./pages/SystemSettingsPage'))
const RecoveryPage = lazy(() => import('./pages/RecoveryPage'))
const CameraSettingsPage = lazy(() => import('./pages/CameraSettingsPage'))

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
  useEffect(() => {
    if (!token) { destroyAllTerminals(); return }
    const check = () => {
      if (!document.hidden) void apiCheckSession().catch(() => { /* Only HTTP 401 expires the login. */ })
    }
    check()
    window.addEventListener('online', check)
    document.addEventListener('visibilitychange', check)
    return () => {
      window.removeEventListener('online', check)
      document.removeEventListener('visibilitychange', check)
    }
  }, [token])
  if (!token) return <Navigate to="/login" replace />
  return <>{children}</>
}

const navigation = [
  { to: '/', label: '程序管理', icon: 'M3 3h7v7H3z M14 3h7v7h-7z M3 14h7v7H3z M14 14h7v7h-7z' },
  { to: '/live-view', label: '实时画面', icon: 'M3 4h18v13H3z M8 21h8 M12 17v4 M9.5 8l5 2.5-5 2.5z' },
  { to: '/image-inference', label: '图片推理', icon: 'M3 3h18v18H3z M4 17l5-5 4 4 3-3 5 5 M16 8h.01' },
  { to: '/video-capture', label: '视频采集', icon: 'M3 6h12v12H3z M15 10l6-4v12l-6-4z' },
  { to: '/dataset', label: '数据集采集', icon: 'M2 7l10-5 10 5-10 5z M2 12l10 5 10-5 M2 17l10 5 10-5' },
  { to: '/services', label: '系统服务', icon: 'M3 3h18v7H3z M3 14h18v7H3z M6 6.5h2 M6 17.5h2' },
  { to: '/system-settings', label: '系统设置', icon: 'M4 6h6m4 0h6 M4 12h10m4 0h2 M4 18h2m4 0h10 M10 4v4 M15 10v4 M7 16v4' },
  { to: '/recovery', label: '故障恢复', icon: 'M3 5v6h6 M3 11a9 9 0 1 1 2.5 8 M12 7v5l3 2' },
  { to: '/camera-settings', label: '摄像头配置', icon: 'M3 7h4l2-3h6l2 3h4v13H3z M16 13a4 4 0 1 1-8 0 4 4 0 1 1 8 0z' },
  { to: '/terminal', label: '终端命令行', icon: 'M3 4h18v16H3z M6 8l4 4-4 4 M13 16h4' },
]

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

        {navigation.map(({ to, label, icon }) =>
          <NavLink key={to} to={to} end={to === '/'} onClick={guardNav}
            className={({ isActive }) => isActive ? 'nav-item active' : 'nav-item'}>
            <svg className="nav-icon" viewBox="0 0 24 24" aria-hidden="true" focusable="false">
              <path d={icon} />
            </svg>
            {label}
          </NavLink>)}

        {/* Bottom: user info + logout */}
        <div className="sidebar-footer">
          <span className="sidebar-user">👤 {username}</span>
          <button className="sidebar-logout" onClick={handleLogout}>退出登录</button>
        </div>
      </nav>

      <main className="main-content">
        <ErrorBoundary>
          <Suspense fallback={<div role="status">正在加载页面…</div>}>
          <Routes>
            <Route path="/"                  element={<AppsPage />} />
            <Route path="/live-view"         element={<LiveViewPage />} />
            <Route path="/image-inference" element={<ImageInferencePage />} />
            <Route path="/video-capture"     element={<VideoCapturePage />} />
            <Route path="/dataset"           element={<DatasetPage />} />
            <Route path="/editor/:appName"   element={<EditorPage />} />
            <Route path="/logs/:appName"     element={<LogsPage />} />
            <Route path="/records/:appName"  element={<RecordsPage />} />
            <Route path="/terminal"        element={<TerminalPage />} />
            <Route path="/services"        element={<ServicesPage />} />
            <Route path="/system-settings" element={<SystemSettingsPage />} />
            <Route path="/recovery" element={<Navigate to="/recovery/system" replace />} />
            <Route path="/recovery/:section" element={<RecoveryPage />} />
            <Route path="/camera-settings" element={<CameraSettingsPage />} />
          </Routes>
          </Suspense>
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
