import { useEffect, useRef, useState } from 'react'
import {
  applyApplicationRecovery, fetchRecoveryStatus, saveApplicationRecovery,
  type ApplicationRecoveryPolicy, type RecoveryStatus,
} from '../../api/client'
import './RecoverySettingsSection.css'
import RecoveryHistory from './RecoveryHistory'
import { readSuccessMessage, readTimestamp } from './recoveryFeedback'

const fields = [
  ['restart_delay_sec', '退出后等待多久重启（秒）', 5, 300],
  ['task_timeout_sec', '单次处理多久未完成视为卡住（秒）', 30, 1800],
  ['startup_timeout_sec', '启动或加载配置最多等待（秒）', 60, 1800],
] as const
const appPolicy = (status: RecoveryStatus): ApplicationRecoveryPolicy => ({
  app_restart_enabled: status.policy.app_restart_enabled,
  app_stall_enabled: status.policy.app_stall_enabled,
  restart_delay_sec: status.policy.restart_delay_sec,
  task_timeout_sec: status.policy.task_timeout_sec,
  startup_timeout_sec: status.policy.startup_timeout_sec,
})
type Draft = { policy: ApplicationRecoveryPolicy; original: ApplicationRecoveryPolicy; revision: string }
const draftOf = (status: RecoveryStatus): Draft => ({ policy: appPolicy(status), original: appPolicy(status), revision: status.revision })
const errorText = (error: unknown) => {
  const detail = (error as { response?: { data?: { detail?: unknown } } })?.response?.data?.detail
  return typeof detail === 'string' ? detail : error instanceof Error ? error.message : String(error)
}

export default function ApplicationRecoverySettingsSection() {
  const [status, setStatus] = useState<RecoveryStatus | null>(null)
  const [draft, setDraft] = useState<Draft | null>(null)
  const [error, setError] = useState('')
  const [readError, setReadError] = useState('')
  const [message, setMessage] = useState('')
  const [busy, setBusy] = useState('')
  const [readAt, setReadAt] = useState('')
  const advanced = useRef<HTMLDetailsElement>(null)
  const generation = useRef(0)
  const operating = useRef(false)
  const dirty = !!draft && JSON.stringify(draft.policy) !== JSON.stringify(draft.original)
  const conflict = !!status && !!draft && status.revision !== draft.revision

  useEffect(() => {
    let active = true
    let pending = false
    const refresh = async () => {
      if (pending || operating.current) return
      pending = true
      const requestGeneration = generation.current
      try {
        const value = await fetchRecoveryStatus()
        if (active && generation.current === requestGeneration) { setStatus(value); setDraft(current => current && JSON.stringify(current.policy) !== JSON.stringify(current.original) ? current : draftOf(value)); setReadError('') }
      } catch (failure) { if (active && generation.current === requestGeneration) setReadError(errorText(failure)) }
      finally { pending = false }
    }
    void refresh()
    const timer = window.setInterval(refresh, 1000)
    return () => { active = false; window.clearInterval(timer) }
  }, [])

  const perform = async (action: string, operation: () => Promise<RecoveryStatus>) => {
    generation.current += 1
    operating.current = true
    setBusy(action); setError(''); setMessage('')
    try {
      const value = await operation()
      setStatus(value); setDraft(draftOf(value)); setReadError('')
      if (action === 'save') setMessage(value.current && value.application_pending
        ? '设置已保存。下次启动时生效，也可以点击“重启程序并生效”立即应用。'
        : value.current ? '设置已保存。' : '设置已保存，程序下次启动时使用。')
      if (action === 'apply') setMessage('程序已重新启动，已重新检查恢复设置。')
      if (action === 'refresh' && !value.policy_error) { const time = readTimestamp(); setReadAt(time); setMessage(readSuccessMessage(dirty, time)) }
    } catch (failure) { setError(errorText(failure)) }
    finally { generation.current += 1; operating.current = false; setBusy('') }
  }
  const change = (policy: ApplicationRecoveryPolicy) => {
    if (draft) { setDraft({ ...draft, policy }); setMessage(''); setError('') }
  }
  const save = () => {
    if (!draft) return
    if (draft.policy.app_stall_enabled && !draft.policy.app_restart_enabled) {
      setError('请同时开启“程序退出后自动重启”，才能在发现卡住后自动恢复。')
      return
    }
    for (const [key, label, min, max] of fields) {
      if (!Number.isInteger(draft.policy[key]) || draft.policy[key] < min || draft.policy[key] > max) {
        setError(`${label}必须为 ${min}～${max} 之间的整数`)
        if (advanced.current) advanced.current.open = true
        return
      }
    }
    void perform('save', () => saveApplicationRecovery(draft.policy, draft.revision))
  }

  const current = status?.current
  const restartLabel = !current ? '等待程序启动' : current.restart_enabled == null ? '状态待确认'
    : current.restart_enabled ? '已生效' : '未开启'
  const stallLabel = !current ? '等待程序启动' : current.watchdog_active ? '已生效'
    : status?.application_pending ? '等待应用设置' : status?.policy.app_stall_enabled ? '未生效' : '未开启'
  const unsupported = current && status?.policy.app_stall_enabled && !status.application_pending && !current.watchdog_active

  return <section className="device-settings-card recovery-settings">
    <div className="device-settings-card-head"><div><h4>程序故障恢复</h4></div></div>
    {(error || readError) && <div className="device-settings-message err">{error || readError}</div>}
    {message && <div className="device-settings-message ok">{message}</div>}
    {!status && !readError && <p>正在读取程序恢复设置……</p>}
    {status && <>
      <div className="recovery-facts recovery-overview">
        <div><span>当前视觉程序</span><strong>{current ? `${current.app}${current.recovery_state === 'restarting' ? '（正在自动恢复）' : ''}` : '未运行'}</strong><small>{current ? `使用配置：${current.config}` : '可先保存设置，程序下次启动时使用'}</small></div>
        <div><span>当前实际保护</span><strong>退出重启：{restartLabel}</strong><small>卡住恢复：{stallLabel}</small></div>
      </div>
      {status.policy_error && <div className="device-settings-message err">恢复设置读取异常：{status.policy_error}</div>}
      {unsupported && <div className="device-settings-note warning">当前程序未能启用卡住检测，程序包可能不支持此功能。退出后重启仍按实际设置执行；如需卡住恢复，请更新支持此功能的程序包后重新启动。</div>}
      {conflict && <div className="device-settings-note warning">设置已在其他页面更新，当前编辑内容已保留。请重新读取后确认。<button className="settings-ghost-btn" disabled={!!busy} onClick={() => void perform('refresh', fetchRecoveryStatus)}>{busy === 'refresh' ? '读取中……' : dirty ? '放弃修改并重新读取' : '重新读取设置'}</button></div>}
      {draft && <>
        <div className="recovery-switches recovery-basic">
          <label className="recovery-switch-row"><input type="checkbox" checked={draft.policy.app_restart_enabled} disabled={!!busy} onChange={event => change({ ...draft.policy, app_restart_enabled: event.target.checked, app_stall_enabled: event.target.checked ? draft.policy.app_stall_enabled : false })} />
            <span><b>程序退出后自动重启</b><small>异常退出后等待 {draft.policy.restart_delay_sec} 秒重启；手动停止不会拉起。</small></span>
          </label>
          <label className="recovery-switch-row"><input type="checkbox" checked={draft.policy.app_stall_enabled} disabled={!!busy} onChange={event => change({ ...draft.policy, app_stall_enabled: event.target.checked, app_restart_enabled: event.target.checked ? true : draft.policy.app_restart_enabled })} />
            <span><b>程序卡住后自动恢复</b><small>长时间未完成处理时重启；同时开启退出重启，需程序包支持。</small></span>
          </label>
        </div>
        <p className="recovery-help">等待输入、主动暂停不算卡住；此处仅重启程序。</p>
        <details className="recovery-advanced" ref={advanced}>
          <summary>高级设置</summary>
          <div className="recovery-details-content">
            <div className="recovery-fields">{fields.map(([key, label, min, max]) => <label key={key}><span>{label}</span><input type="number" min={min} max={max} step={1} disabled={!!busy} value={draft.policy[key]} onChange={event => change({ ...draft.policy, [key]: Number(event.target.value) })} /></label>)}</div>
            <div className="settings-actions recovery-agent-actions">
              <button className="settings-ghost-btn" disabled={!!busy} onClick={() => change({ ...draft.policy, restart_delay_sec: 5, task_timeout_sec: 120, startup_timeout_sec: 300 })}>恢复推荐时间</button>
              <button className="settings-ghost-btn" disabled={!!busy} title="读取设备上已保存的设置、当前状态和恢复记录；未保存的修改会被放弃。" onClick={() => void perform('refresh', fetchRecoveryStatus)}>{busy === 'refresh' ? '读取中……' : dirty ? '放弃修改并重新读取' : '重新读取设置'}</button>
              {readAt && <span className="recovery-help recovery-read-feedback" role="status">上次手动读取：{readAt}</span>}
            </div>
          </div>
        </details>
        {current && status.application_pending && <div className="device-settings-note warning">当前程序还在使用启动时的设置。{dirty ? '先保存修改，再应用到当前程序。' : '可以等下次启动生效，也可以立即应用。重启程序会短暂中断视频和推理。'}</div>}
        <div className="settings-actions recovery-agent-actions">
          <button className={current && status.application_pending && !dirty ? 'settings-ghost-btn' : 'settings-primary-btn'} disabled={!!busy || !dirty || conflict || !!status.policy_error} onClick={save}>{busy === 'save' ? '保存中……' : '保存设置'}</button>
          {current && status.application_pending && <button className="settings-primary-btn" disabled={!!busy || dirty || conflict || !!status.policy_error} onClick={() => void perform('apply', () => applyApplicationRecovery(draft.revision, current.app))}>{busy === 'apply' ? '正在重启程序……' : '重启程序并生效'}</button>}
        </div>
      </>}
      <RecoveryHistory history={status.history} category="application" title="最近程序恢复记录" />
    </>}
  </section>
}
