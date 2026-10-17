import PerformanceMonitor from './PerformanceMonitor'
import { useDeviceInformation } from './useDeviceInformation'
import './RecoverySettingsSection.css'

export default function PerformanceSection() {
  const { information, setInformation, fetching, error } = useDeviceInformation(1000)
  const errors = information?.errors.filter(item => !/^(网卡|网络|IPv4|IPv6|存储|关键服务|时间同步|当前程序|板卡信息|系统概况|storage|application)/.test(item)) || []

  return <section className="device-settings-card recovery-settings performance-information">
    <div className="device-settings-card-head"><h4>性能</h4>
    </div>
    {error && <div className="device-settings-message err">性能信息读取失败：{error}{information ? '。当前显示上次读取的信息。' : ''}</div>}
    {information ? <>
      <PerformanceMonitor information={information} onSaved={performance => setInformation(previous => previous ? { ...previous, performance } : previous)} />
      {!!errors.length && <details className="device-information-capabilities"><summary>部分信息暂不可用（{errors.length} 项）</summary>
        <ul>{errors.map(item => <li key={item}>{item}</li>)}</ul></details>}
    </> : !error && <p>{fetching ? '正在读取性能信息……' : '尚未取得性能信息，将自动重试。'}</p>}
  </section>
}
