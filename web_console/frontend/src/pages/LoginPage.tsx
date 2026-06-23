import { useState, useEffect, useRef } from 'react'
import { useNavigate } from 'react-router-dom'
import { apiLogin } from '../api/client'
import { useAuthStore } from '../store/authStore'
import './LoginPage.css'

export default function LoginPage() {
  const navigate   = useNavigate()
  const setAuth    = useAuthStore(s => s.setAuth)
  const token      = useAuthStore(s => s.token)

  const [username,   setUsername]   = useState('')
  const [password,   setPassword]   = useState('')
  const [error,      setError]      = useState('')
  const [loading,    setLoading]    = useState(false)
  const [logoFailed, setLogoFailed] = useState(false)
  // 每次打开随机取一张(frontend/logos/ 里的图片或 GIF; 空则回退 logo.png)；?t= 触发每次重新随机
  const logoSrc = useRef(`/logo/random?t=${Date.now()}`).current

  useEffect(() => {
    if (token) navigate('/', { replace: true })
  }, [token, navigate])

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault()
    setError('')
    setLoading(true)
    try {
      const data = await apiLogin(username, password)
      setAuth(data.token, data.username)
      navigate('/', { replace: true })
    } catch {
      setError('用户名或密码错误')
    } finally {
      setLoading(false)
    }
  }

  return (
    <div className="login-page">
      <div className="login-card">
        {logoFailed
          ? <div className="login-logo">RK<br/>3588</div>
          : <img src={logoSrc} className="login-logo-img" alt="Logo"
                 onError={() => setLogoFailed(true)} />
        }
        <h1 className="login-title">AI 视觉配置平台</h1>

        <form className="login-form" onSubmit={handleSubmit}>
          <div className="login-field">
            <label>用户名</label>
            <input
              type="text"
              value={username}
              onChange={e => setUsername(e.target.value)}
              autoComplete="off"
            />
          </div>
          <div className="login-field">
            <label>密码</label>
            <input
              type="password"
              value={password}
              onChange={e => setPassword(e.target.value)}
              autoComplete="new-password"
            />
          </div>

          {error && <div className="login-error">⚠ {error}</div>}

          <button
            type="submit"
            className="login-btn"
            disabled={loading || !username || !password}
          >
            {loading ? '验证中…' : '登  录'}
          </button>
        </form>

      </div>
    </div>
  )
}
