import { useCallback, useEffect, useRef, useState } from 'react'
import { fetchApps, type AppInfo } from '../api/client'
import ServicesPanel from '../components/ServicesPanel'
import './ServicesPage.css'

export default function ServicesPage() {
  const [apps, setApps] = useState<AppInfo[]>([])
  const [loading, setLoading] = useState(true)
  const [toast, setToast] = useState<{ msg: string; type: 'ok' | 'err' } | null>(null)
  const toastTimer = useRef<ReturnType<typeof setTimeout> | null>(null)

  useEffect(() => {
    let active = true
    const load = () => fetchApps().then(data => { if (active) setApps(data) })
      .catch(() => { if (active) setToast({ msg: '加载程序包列表失败', type: 'err' }) })
      .finally(() => { if (active) setLoading(false) })
    load()
    const timer = setInterval(load, 5000)
    return () => { active = false; clearInterval(timer) }
  }, [])

  const showToast = useCallback((msg: string, type: 'ok' | 'err' = 'ok') => {
    if (toastTimer.current) clearTimeout(toastTimer.current)
    setToast({ msg, type })
    if (type === 'ok') toastTimer.current = setTimeout(() => setToast(null), 3000)
  }, [])

  return <div className="services-page">
    <div className="services-page-header"><div>
      <h2>系统服务</h2>
      <p>管理后台进程的启动、停止和运行状态。连接、契约和 OTA 参数请从对应程序包画布的“应用集成”进入。</p>
    </div></div>
    {toast && <div className={`services-toast ${toast.type}`}><span>{toast.msg}</span></div>}
    {loading ? <div className="services-page-state">正在加载……</div>
      : <ServicesPanel apps={apps} onToast={showToast} />}
  </div>
}
