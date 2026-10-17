import { useCallback, useRef, useState } from 'react'
import { fetchApps, type AppInfo } from '../api/client'
import ServicesPanel from '../components/ServicesPanel'
import './ServicesPage.css'
import { useAutoSync } from '../hooks/useAutoSync'
import ConfigSyncNotice from '../components/ConfigSyncNotice'

export default function ServicesPage() {
  const [apps, setApps] = useState<AppInfo[]>([])
  const [loading, setLoading] = useState(true)
  const [toast, setToast] = useState<{ msg: string; type: 'ok' | 'err' } | null>(null)
  const toastTimer = useRef<ReturnType<typeof setTimeout> | null>(null)

  const syncError = useAutoSync(fetchApps, data => { setApps(data); setLoading(false) })

  const showToast = useCallback((msg: string, type: 'ok' | 'err' = 'ok') => {
    if (toastTimer.current) clearTimeout(toastTimer.current)
    setToast({ msg, type })
    if (type === 'ok') toastTimer.current = setTimeout(() => setToast(null), 3000)
  }, [])

  return <div className="services-page">
    <div className="services-page-header"><div>
      <h2>系统服务</h2>
    </div></div>
    {toast && <div className={`services-toast ${toast.type}`}><span>{toast.msg}</span></div>}
    <ConfigSyncNotice error={syncError} />
    {loading && !syncError ? <div className="services-page-state">正在加载……</div>
      : <ServicesPanel apps={apps} onToast={showToast} />}
  </div>
}
