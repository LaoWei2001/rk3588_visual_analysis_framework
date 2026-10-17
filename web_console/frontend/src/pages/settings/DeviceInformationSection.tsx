import type { ReactNode } from 'react'
import './RecoverySettingsSection.css'
import { useDeviceInformation } from './useDeviceInformation'

const formatTime = (stamp?: number | null) => stamp != null ? new Date(stamp * 1000).toLocaleString('zh-CN', { hour12: false }) : '未提供'
const duration = (value?: number | null) => {
  if (value == null) return '未提供'
  const days = Math.floor(value / 86400), hours = Math.floor(value / 3600) % 24, minutes = Math.floor(value / 60) % 60
  return days ? `${days} 天 ${hours} 小时 ${minutes} 分钟` : hours ? `${hours} 小时 ${minutes} 分钟` : minutes ? `${minutes} 分钟` : `${Math.floor(value)} 秒`
}
const field = (label: string, value: ReactNode) => <div key={label}><dt>{label}</dt><dd>{value}</dd></div>
export default function DeviceInformationSection() {
  const { information, fetching, error } = useDeviceInformation(1000)

  const diagnostics = information?.diagnostics
  const board = diagnostics?.board
  const observer = information?.observer
  const system = information?.system
  const cpu = information?.cpu
  const sampleStale = !!board && (!diagnostics?.sample_time || Date.now() / 1000 - diagnostics.sample_time > 180)
  const evidenceError = diagnostics?.error || observer?.error || observer?.diagnostic_error || sampleStale
  const errors = information?.errors.filter(item => !/^(网卡|网络|IPv4|IPv6|存储|关键服务|时间同步|当前程序|storage|application|内存|memory|温度|temperatures|加速设备|accelerators|performance)/.test(item)) || []


  return <section className="device-settings-card recovery-settings device-information">
    <div className="device-settings-card-head"><h4>设备基本信息</h4>
      {information && <small className="recovery-read-feedback">更新于 {formatTime(information.updated_at)}</small>}
    </div>
    {error && <div className="device-settings-message err">设备信息读取失败：{error}{information ? '。当前显示上次读取的信息。' : ''}</div>}
    {information ? <>
      <h5 className="device-information-section-title">系统概况</h5>
      <dl className="device-information-fields">
        {field('设备主机名', system?.hostname || '未提供')}
        {field('板卡型号', board?.model || '未提供型号')}
        {field('芯片型号', system?.soc || '未提供')}
        {field('设备序列号', system?.serial || '未提供')}
        {field('操作系统', system?.os_name || '未提供')}
        {field('内核版本', system?.kernel || board?.kernel || '未提供版本')}
        {field('CPU 架构与核心', `${cpu?.architecture || '未提供'} · ${cpu?.cores == null ? '核心数未提供' : `${cpu.cores} 核`}`)}
        {field('开机时间', formatTime(system?.boot_time))}
        {field('连续运行时间', duration(system?.uptime_sec))}
      </dl>
      {!!errors.length && <details className="device-information-capabilities"><summary>部分信息暂不可用（{errors.length} 项）</summary>
        <ul>{errors.map(item => <li key={item}>{item}</li>)}</ul></details>}
      {board && <details className="device-information-capabilities"><summary>故障取证能力{evidenceError && ' · 异常'}</summary>
        {diagnostics?.error && <div className="device-settings-message err">{diagnostics.error}</div>}
        {observer?.error && <div className="device-settings-message err">{observer.error}</div>}
        {observer?.diagnostic_error && <div className="device-settings-message err">{observer.diagnostic_error}</div>}
        {sampleStale && <div className="device-settings-note warning">取证采样未更新</div>}
        <dl className="device-information-fields">
          {field('设备标识', board.compatible || '未提供')}
          {field('取证采样时间', formatTime(diagnostics?.sample_time))}
          {board.watchdogs && field('看门狗', board.watchdogs.length ? board.watchdogs.map(item => <div key={item.device}>
            {item.device}：{item.identity || '驱动未提供'} · 启动状态 {item.bootstatus ?? '未提供'}
          </div>) : '未提供')}
          {board.pstore != null && field('崩溃残留目录', board.pstore ? '可见' : '未提供')}
          {board.psi && field('压力指标（PSI）', `CPU ${board.psi.cpu ? '可读取' : '未提供'} · 内存 ${board.psi.memory ? '可读取' : '未提供'} · I/O ${board.psi.io ? '可读取' : '未提供'}`)}
          {board.reset_sources && field('复位原因（原始值）', board.reset_sources.length ? board.reset_sources.map(item => <div key={item.source}>{item.source}：{item.value}</div>) : '未提供')}
        </dl>
        {!!board.limitations.length && <ul>{board.limitations.map(item => <li key={item}>{item}</li>)}</ul>}
        {!!diagnostics?.errors?.length && <p>{diagnostics.errors.join('；')}</p>}
      </details>}
    </> : !error && <p>{fetching ? '正在读取设备信息……' : '尚未取得设备信息，将自动重试。'}</p>}
  </section>
}
