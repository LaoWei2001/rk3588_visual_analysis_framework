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
  const [passwordFocused, setPasswordFocused] = useState(false)
  const [passwordVisible, setPasswordVisible] = useState(false)
  const isPasswordCovered = passwordFocused
  const leftKaomoji = isPasswordCovered ? '（*／∇\\*）' : '（＾∀＾）'
  const rightKaomoji = isPasswordCovered ? '（*/∇＼*）' : '✧*｡٩(ˊᗜˋ*)و✧*｡'
  const bottomKaomoji = isPasswordCovered ? '（ > _ < ）' : '（ ◔ ᴗ ◔ ）'
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
      <div className="login-kaomoji login-kaomoji--left" aria-hidden="true">
        <span key={`left-${leftKaomoji}`}>{leftKaomoji}</span>
      </div>
      <div className="login-kaomoji login-kaomoji--right" aria-hidden="true">
        <span key={`right-${rightKaomoji}`}>{rightKaomoji}</span>
      </div>
      <div className="login-kaomoji login-kaomoji--bottom" aria-hidden="true">
        <span key={`bottom-${bottomKaomoji}`}>{bottomKaomoji}</span>
      </div>

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
            <div className="login-password-control">
              <input
                type={passwordVisible ? 'text' : 'password'}
                value={password}
                onChange={e => setPassword(e.target.value)}
                onFocus={() => setPasswordFocused(true)}
                onBlur={() => setPasswordFocused(false)}
                autoComplete="new-password"
              />
              <button
                type="button"
                className="login-password-toggle"
                aria-label={passwordVisible ? '隐藏密码' : '显示密码'}
                aria-pressed={passwordVisible}
                title={passwordVisible ? '隐藏密码' : '显示密码'}
                onMouseDown={e => e.preventDefault()}
                onClick={() => setPasswordVisible(visible => !visible)}
              >
                <svg viewBox="0 0 24 24" aria-hidden="true">
                  <path d="M2.5 12s3.5-6 9.5-6 9.5 6 9.5 6-3.5 6-9.5 6-9.5-6-9.5-6Z" />
                  <circle cx="12" cy="12" r="2.7" />
                  {passwordVisible && <path d="M4 4l16 16" />}
                </svg>
              </button>
            </div>
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

        <div className="login-footer">
          本项目已开源至{' '}
          <a href="https://github.com/LaoWei2001/rk3588_visual_analysis_framework"
             target="_blank" rel="noreferrer">
            GitHub
          </a>
          ，来打颗⭐吧!
        </div>
      </div>
    </div>
  )
}
