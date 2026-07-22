import { useCallback, useEffect, useRef, useState } from 'react'
import { fetchApps, type AppInfo } from '../api/client'
import ServiceConfigModal from '../components/ServiceConfigModal'
import ServicesPanel from '../components/ServicesPanel'
import './ServicesPage.css'

export default function ServicesPage() {
  const [apps, setApps] = useState<AppInfo[]>([])
  const [selectedApp, setSelectedApp] = useState('')
  const [loading, setLoading] = useState(true)
  const [toast, setToast] = useState<{ msg: string; type: 'ok' | 'err' } | null>(null)
  const toastTimer = useRef<ReturnType<typeof setTimeout> | null>(null)

  useEffect(() => {
    fetchApps()
      .then(data => {
        setApps(data)
        setSelectedApp(current => {
          if (current && data.some(app => app.name === current)) return current
          return data.find(app => app.status === 'running')?.name ?? data[0]?.name ?? ''
        })
      })
      .catch(() => setToast({ msg: '加载程序包列表失败', type: 'err' }))
      .finally(() => setLoading(false))
  }, [])

  useEffect(() => () => {
    if (toastTimer.current) clearTimeout(toastTimer.current)
  }, [])

  const showToast = useCallback((msg: string, type: 'ok' | 'err' = 'ok') => {
    if (toastTimer.current) clearTimeout(toastTimer.current)
    setToast({ msg, type })
    if (type === 'ok') {
      toastTimer.current = setTimeout(() => {
        setToast(null)
        toastTimer.current = null
      }, 3000)
    }
  }, [])

  const showConfigToast = useCallback((msg: string, ok = true) => {
    showToast(msg, ok ? 'ok' : 'err')
  }, [showToast])

  return (
    <div className="services-page">
      <div className="services-page-header">
        <div>
          <h2>服务配置</h2>
          <p>管理设备后台服务及其连接参数，不修改任何画布配置文件。</p>
        </div>
      </div>

      {toast && (
        <div className={`services-toast ${toast.type}`}>
          <span>{toast.msg}</span>
          {toast.type === 'err' && <button onClick={() => setToast(null)}>×</button>}
        </div>
      )}

      {loading ? (
        <div className="services-page-state">正在加载……</div>
      ) : (
        <>
          {apps.length === 0 ? (
            <div className="services-page-state">暂无程序包，无法定位服务参数文件。</div>
          ) : (
            <section className="services-scope">
              <div>
                <strong>服务文件所属程序包</strong>
                <p>这里只用于定位 services 目录；下方设置不属于该程序的任何 assets/config*.json。</p>
              </div>
              <select value={selectedApp} onChange={event => setSelectedApp(event.target.value)}>
                {apps.map(app => <option key={app.name} value={app.name}>{app.name}</option>)}
              </select>
            </section>
          )}

          <ServicesPanel apps={apps} onToast={showToast} />

          {selectedApp && (
            <ServiceConfigModal
              key={selectedApp}
              appName={selectedApp}
              embedded
              onClose={() => {}}
              onToast={showConfigToast}
            />
          )}
        </>
      )}
    </div>
  )
}
