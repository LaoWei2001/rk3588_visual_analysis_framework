import type { RecoveryHistoryEvent, RecoveryHistorySnapshot } from '../../api/client'

const results = { pending: '等待结果确认', performed: '已执行', recovered: '已确认恢复', failed: '恢复失败', unknown: '结果未确认' }
const categories = { system: '系统', network: '网络', application: '程序' }
const stageNames: Record<string, string> = { board_reboot_request: '发起重启', system_shutdown: '进入正常关机流程', system_boot: '设备已重新启动' }
const formatTime = (time: number) => new Date(time * 1000).toLocaleString('zh-CN', { hour12: false })

export function isFaultRecoveryEvent(event: RecoveryHistoryEvent) {
  if (event.fault_recovery != null) return event.fault_recovery
  // Compatibility with an older server during an upgrade.
  if (event.category === 'system') return ['system_boot', 'board_reboot_request'].includes(event.kind)
    && event.source !== '每日定时重启'
    && !/^(到达每日定时重启时间|上一运行周期进入正常关机流程)/.test(event.reason)
  if (event.category === 'network') return ['reconnect', 'restart_manager', 'reboot', 'reboot_requested', 'board_reboot_request', 'action_error', 'external_recovery', 'network_rollback'].includes(event.kind)
  return ['service_failure', 'application_restart', 'restore_error', 'service_error'].includes(event.kind)
}

export function RecoveryEventList({ events, operation = false }: { events: RecoveryHistoryEvent[]; operation?: boolean }) {
  return <ul>{events.map(event => <li key={event.id}>
    <div className="recovery-history-time"><time>{formatTime(event.time)}</time><small>{event.time_basis}</small></div>
    <div className="recovery-history-body">
      <div className="recovery-history-heading"><b>{categories[event.category]}</b><span data-result={event.result}>{results[event.result]}</span></div>
      <div><b>{operation ? '事由：' : '原因：'}</b>{event.reason}</div>
      {!operation && event.diagnosis && <div><b>故障判断：</b>{{ confirmed: '触发证据确认', probable: '综合迹象推测', unknown: '证据不足' }[event.diagnosis.confidence]}
        {!!event.diagnosis.findings.length && <div>相关异常：{event.diagnosis.findings.map(item => item.summary).join('、')}</div>}
        {!event.diagnosis.recovery && event.diagnosis.limitations.map(item => <small key={item}>{item}<br /></small>)}
      </div>}
      {!operation && event.diagnosis?.recovery ? <div className="recovery-method-assessment">
        <div><b>恢复方式：</b>{event.diagnosis.recovery.method}</div>
        <div><b>恢复判断：</b>{{ confirmed: '硬件证据确认', probable: '有依据的推测', unknown: '复位来源证据不足' }[event.diagnosis.recovery.confidence]}</div>
        {event.diagnosis.recovery.basis.map(item => <div key={item}>依据：{item}</div>)}
        {event.diagnosis.recovery.limitations.map(item => <div key={item}><small>限制：{item}</small></div>)}
      </div> : <div><b>{operation ? '执行方式：' : '恢复方式：'}</b>{event.action}</div>}
      <div><b>结果：</b>{event.detail || results[event.result]}</div>
      <small>记录来源：{event.source}{event.updated_at - event.time > 1 ? ` · 结果更新：${formatTime(event.updated_at)}` : ''}</small>
      {!!event.stages?.length && <details className="recovery-history-evidence recovery-history-stages">
        <summary>重启过程（{event.stages.length} 个阶段）</summary>
        {event.stages.map(stage => <div key={stage.id} className="recovery-history-stage">
          <div><time>{formatTime(stage.time)}</time> · {stageNames[stage.kind] || stage.action}</div>
          <small>{stage.action} · {stage.source}</small>
        </div>)}
      </details>}
      {!!event.evidence && <details className="recovery-history-evidence"><summary>{operation ? '记录依据' : '判断依据'}</summary><pre>{event.evidence}</pre></details>}
    </div>
  </li>)}</ul>
}

export default function RecoveryHistory({ history, category, title }: {
  history?: RecoveryHistorySnapshot
  category: 'system' | 'network' | 'application'
  title: string
}) {
  const events = history?.events.filter(event => event.category === category && isFaultRecoveryEvent(event)) ?? []
  const observer = history?.observer
  const stale = !!history && (!observer?.updated_at || Date.now() / 1000 - observer.updated_at > 60)
  return <details className="recovery-events recovery-history">
    <summary>{title}{!!events.length && `（${events.length} 条）`}</summary>
    <p>每类只保留最新 {history?.keep_per_category ?? 20} 条{category === 'system' ? '，正常重启见“运行与操作记录”' : ''}。</p>
    {history?.error && <div className="device-settings-message err">恢复记录读取失败：{history.error}</div>}
    {observer?.error && <div className="device-settings-message err">恢复记录服务异常：{observer.error}</div>}
    {stale && <div className="device-settings-note warning">尚未确认记录服务正在运行，新恢复事件可能未被记录。</div>}
    {!!events.length ? <RecoveryEventList events={events} /> : !history?.error && <p>暂无恢复记录。</p>}
  </details>
}
