import { Navigate, useNavigate, useParams } from 'react-router-dom'
import RecoverySettingsSection from './settings/RecoverySettingsSection'
import NetworkRecoverySettingsSection from './settings/NetworkRecoverySettingsSection'
import ApplicationRecoverySettingsSection from './settings/ApplicationRecoverySettingsSection'
import OperationHistorySection from './settings/OperationHistorySection'
import './SystemSettingsPage.css'

const sections = [
  { id: 'system', icon: '↻', title: '系统故障恢复', hint: '整机卡死与系统崩溃' },
  { id: 'network', icon: '⌁', title: '网络故障恢复', hint: '有线与 Wi-Fi 自愈' },
  { id: 'application', icon: '▷', title: '程序故障恢复', hint: '程序退出与卡住恢复' },
  { id: 'operations', icon: '≡', title: '运行与操作记录', hint: '正常启动、定时与人工操作' },
] as const

export default function RecoveryPage() {
  const { section } = useParams()
  const navigate = useNavigate()
  if (!sections.some(item => item.id === section)) return <Navigate to="/recovery/system" replace />

  return (
    <div className="system-settings-page">
      <header className="system-settings-header">
        <h2>故障恢复</h2>
      </header>
      <div className="device-settings-layout">
        <nav className="device-settings-nav" aria-label="故障恢复分类">
          {sections.map(item => (
            <button key={item.id} type="button" className={section === item.id ? 'active' : ''}
              aria-current={section === item.id ? 'page' : undefined}
              onClick={() => navigate(`/recovery/${item.id}`)}>
              <span className="device-settings-nav-icon">{item.icon}</span>
              <span><b>{item.title}</b><small>{item.hint}</small></span>
            </button>
          ))}
        </nav>
        <main className="device-settings-content">
          {/* Keep forms mounted so switching categories retains unsaved drafts. */}
          <div hidden={section !== 'system'}><RecoverySettingsSection /></div>
          <div hidden={section !== 'network'}><NetworkRecoverySettingsSection /></div>
          <div hidden={section !== 'application'}><ApplicationRecoverySettingsSection /></div>
          <div hidden={section !== 'operations'}><OperationHistorySection active={section === 'operations'} /></div>
        </main>
      </div>
    </div>
  )
}
