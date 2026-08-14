import { useState } from 'react'
import StorageSettingsSection from './settings/StorageSettingsSection'
import NetworkSettingsSection from './settings/NetworkSettingsSection'
import SystemSettingsSection from './settings/SystemSettingsSection'
import './SystemSettingsPage.css'

type Section = 'storage' | 'network' | 'system'

const sections: Array<{ id: Section; icon: string; title: string; hint: string }> = [
  { id: 'storage', icon: '▰', title: '存储', hint: '磁盘与事件数据' },
  { id: 'network', icon: '⌁', title: '网络', hint: '网卡与 IPv4' },
  { id: 'system', icon: '⚙', title: '系统', hint: '时间与定时重启' },
]

export default function SystemSettingsPage() {
  const [section, setSection] = useState<Section>('system')

  return (
    <div className="system-settings-page">
      <header className="system-settings-header">
        <h2>系统设置</h2>
        <p>管理盒子的存储、网络和系统级运行参数。</p>
      </header>

      <div className="device-settings-layout">
        <nav className="device-settings-nav" aria-label="系统设置分类">
          {sections.map(item => (
            <button
              key={item.id}
              className={section === item.id ? 'active' : ''}
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
        </main>
      </div>
    </div>
  )
}
