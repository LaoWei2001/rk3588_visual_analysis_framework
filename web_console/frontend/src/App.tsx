import { useState, useRef } from 'react'
import { BrowserRouter, Routes, Route, NavLink, Navigate, useNavigate } from 'react-router-dom'
import AppsPage    from './pages/AppsPage'
import EditorPage  from './pages/EditorPage'
import LogsPage    from './pages/LogsPage'
import RecordsPage from './pages/RecordsPage'
import LoginPage   from './pages/LoginPage'
import TerminalPage from './pages/TerminalPage'
import { destroyAllTerminals } from './pages/terminalSession'
import ErrorBoundary from './components/ErrorBoundary'
import { useAuthStore } from './store/authStore'
import { apiLogout }    from './api/client'
import './App.css'

// ── Sidebar logo: image on top, text below; no fallback if image missing ──
function SidebarLogo() {
  const [failed, setFailed] = useState(false)
  const src = useRef(`/logo.png?t=${Date.now()}`).current
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
    destroyAllTerminals()  // 主动断开所有终端 WebSocket，让板端回收 shell
    try { await apiLogout() } catch { /* token already expired is fine */ }
    clearAuth()
    navigate('/login', { replace: true })
  }

  return (
    <div className="app-shell">
      <nav className="sidebar">
        <SidebarLogo />

        <NavLink to="/" end className={({ isActive }) => isActive ? 'nav-item active' : 'nav-item'}>
          <span className="nav-icon">▣</span> 程序管理
        </NavLink>

        <NavLink to="/terminal" className={({ isActive }) => isActive ? 'nav-item active' : 'nav-item'}>
          <span className="nav-icon">▤</span> 终端
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
            <Route path="/editor/:appName"   element={<EditorPage />} />
            <Route path="/logs/:appName"     element={<LogsPage />} />
            <Route path="/records/:appName"  element={<RecordsPage />} />
            <Route path="/terminal"        element={<TerminalPage />} />
          </Routes>
        </ErrorBoundary>
      </main>
    </div>
  )
}

// ── Root ──────────────────────────────────────────────────────────────────
export default function App() {
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
