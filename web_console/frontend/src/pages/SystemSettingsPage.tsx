import { useSearchParams } from 'react-router-dom'
import StorageSettingsSection from './settings/StorageSettingsSection'
import NetworkSettingsSection from './settings/NetworkSettingsSection'
import SystemSettingsSection from './settings/SystemSettingsSection'
import DeviceInformationSection from './settings/DeviceInformationSection'
import PerformanceSection from './settings/PerformanceSection'
import './SystemSettingsPage.css'

type Section = 'storage' | 'network' | 'system' | 'device' | 'performance'

const sections: Array<{ id: Section; icon: string; title: string; hint: string }> = [
  { id: 'storage', icon: '▰', title: '存储', hint: '磁盘与告警记录' },
  { id: 'network', icon: '⌁', title: '网络', hint: '网卡与 IPv4' },
  { id: 'system', icon: '⚙', title: '系统', hint: '时间与定时重启' },
  { id: 'performance', icon: '▥', title: '性能', hint: '利用率、频率与温度' },
  { id: 'device', icon: 'ⓘ', title: '设备基本信息', hint: '板卡、系统与取证能力' },
]

export default function SystemSettingsPage() {
  const [params, setParams] = useSearchParams()
  const selected = params.get('section')
  const section: Section = sections.some(item => item.id === selected) ? selected as Section : 'system'
  const setSection = (value: Section) => {
    const next = new URLSearchParams(params)
    next.set('section', value)
    setParams(next)
  }

  return (
    <div className="system-settings-page">
      <header className="system-settings-header">
        <h2>系统设置</h2>
      </header>

      <div className="device-settings-layout">
        <nav className="device-settings-nav" aria-label="系统设置分类">
          {sections.map(item => (
            <button
              key={item.id}
              className={section === item.id ? 'active' : ''}
              aria-current={section === item.id ? 'page' : undefined}
              onClick={() => setSection(item.id)}
            >
              <span className="device-settings-nav-icon">{item.icon}</span>
              <span><b>{item.title}</b><small>{item.hint}</small></span>
            </button>
          ))}
        </nav>

        <main className="device-settings-content">
          {section === 'storage' && <StorageSettingsSection />}
          {section === 'network' && <NetworkSettingsSection />}
          {section === 'system' && <SystemSettingsSection />}
          {section === 'device' && <DeviceInformationSection />}
          {section === 'performance' && <PerformanceSection />}
        </main>
      </div>
    </div>
  )
}
