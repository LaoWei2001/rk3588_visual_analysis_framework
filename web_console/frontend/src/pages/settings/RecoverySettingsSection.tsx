import { useEffect, useRef, useState } from 'react'
import { fetchRecoveryStatus, saveSystemRecovery, type RecoveryStatus, type SystemRecoveryPolicy } from '../../api/client'
import './RecoverySettingsSection.css'
import RecoveryHistory from './RecoveryHistory'
import { readSuccessMessage, readTimestamp } from './recoveryFeedback'

const errorText = (error: unknown) => {
  const detail = (error as { response?: { data?: { detail?: unknown } } })?.response?.data?.detail
  return typeof detail === 'string' ? detail : error instanceof Error ? error.message : String(error)
}
const displayDuration = (value: string) => value.replace(/(\d+(?:\.\d+)?)min\b/g, '$1 分钟').replace(/(\d+(?:\.\d+)?)s\b/g, '$1 秒')
type Draft = { policy: SystemRecoveryPolicy; original: SystemRecoveryPolicy; revision: string }
const draftOf = (status: RecoveryStatus): Draft | null => {
  if (!status.system?.policy || !status.system.revision) return null
  const policy = { ...status.system.policy }
  const panicTimeout = status.hardware.panic_timeout_sec
  // Panic settings apply immediately; follow live sysctl changes without persisting them.
  // Hardware settings remain staged until reboot, including pending switch changes.
  if (panicTimeout != null) {
    policy.panic_enabled = panicTimeout !== 0
    if (panicTimeout !== 0) policy.panic_timeout_sec = panicTimeout
  }
  return { policy, original: { ...policy }, revision: status.system.revision }
}
const changed = (draft: Draft) => JSON.stringify(draft.policy) !== JSON.stringify(draft.original)

export default function RecoverySettingsSection() {
  const [status, setStatus] = useState<RecoveryStatus | null>(null)
  const [readError, setReadError] = useState('')
  const [draft, setDraft] = useState<Draft | null>(null)
  const [busy, setBusy] = useState(false)
  const [reloading, setReloading] = useState(false)
  const [readAt, setReadAt] = useState('')
  const [error, setError] = useState('')
  const [message, setMessage] = useState('')
  const generation = useRef(0)
  const operating = useRef(false)
  const dirty = !!draft && changed(draft)
  const needsSave = !!draft && !!status?.system?.policy
    && JSON.stringify(draft.policy) !== JSON.stringify(status.system.policy)
  const conflict = !!draft && !!status?.system?.revision && draft.revision !== status.system.revision

  useEffect(() => {
    let active = true
    let pending = false
    const refresh = async () => {
      if (pending || operating.current) return
      pending = true
      const requestGeneration = generation.current
      try {
        const value = await fetchRecoveryStatus()
        if (active && generation.current === requestGeneration) {
          setStatus(value); setReadError('')
          setDraft(current => current && changed(current) ? current : draftOf(value))
        }
      } catch (failure) { if (active && generation.current === requestGeneration) setReadError(errorText(failure)) }
      finally { pending = false }
    }
    void refresh()
    const timer = window.setInterval(refresh, 1000)
    return () => { active = false; window.clearInterval(timer) }
  }, [])

  const change = <K extends keyof SystemRecoveryPolicy>(key: K, value: SystemRecoveryPolicy[K]) => {
    if (draft) { setDraft({ ...draft, policy: { ...draft.policy, [key]: value } }); setError(''); setMessage('') }
  }
  const save = async () => {
    if (!draft) return
    for (const [key, label] of [['watchdog_timeout_sec', '卡死复位等待时间'], ['reboot_timeout_sec', '重启阶段看门狗超时']] as const) {
      if (!Number.isInteger(draft.policy[key]) || draft.policy[key] < 1 || draft.policy[key] > 86400) {
        setError(`${label}必须为 1～86400 秒的整数`)
        return
      }
    }
    if (!Number.isInteger(draft.policy.panic_timeout_sec) || draft.policy.panic_timeout_sec === 0
      || draft.policy.panic_timeout_sec < -2147483648 || draft.policy.panic_timeout_sec > 2147483647) {
      setError('崩溃重启等待时间必须为 0～2147483647 秒的整数，0 表示立即重启')
      return
    }
    generation.current += 1
    operating.current = true
    setBusy(true); setError(''); setMessage('')
    try {
      const value = await saveSystemRecovery(draft.policy, draft.revision)
      setStatus(value); setDraft(draftOf(value)); setReadError('')
      setMessage(value.system?.hardware_pending
        ? '设置已保存。系统崩溃设置已生效；硬件看门狗设置等待你正常重启设备后生效，当前不会自动重启。'
        : '系统恢复设置已保存，当前生效状态见上方。')
    } catch (failure) { setError(errorText(failure)) }
    finally { generation.current += 1; operating.current = false; setBusy(false) }
  }
  const reload = async () => {
    generation.current += 1
    operating.current = true
    setBusy(true); setReloading(true); setError(''); setMessage('')
    try {
      const value = await fetchRecoveryStatus()
      setStatus(value); setDraft(draftOf(value)); setReadError('')
      if (draftOf(value)) { const time = readTimestamp(); setReadAt(time); setMessage(readSuccessMessage(dirty, time)) }
    } catch (failure) { setReadError(errorText(failure)) }
    finally { generation.current += 1; operating.current = false; setBusy(false); setReloading(false) }
  }

  const hardware = status?.hardware
  const hardwareDisabled = !!status?.system?.configured && status.system.policy?.hardware_enabled === false && !hardware?.enabled && !hardware?.error
  const hardwareLabel = readError ? '状态未更新' : hardwareDisabled ? '已关闭' : hardware?.label || (!hardware?.enabled ? '未确认开启' : hardware.verification_state === 'failed' ? '需检查' : '已开启')
  const panicTimeout = status?.hardware.panic_timeout_sec
  const panicEnabled = panicTimeout != null && panicTimeout !== 0

  return (
    <section className="device-settings-card recovery-settings">
      <div className="device-settings-card-head">
        <div><h4>系统故障恢复</h4></div>
      </div>
      {readError && <div className="device-settings-message err">状态读取失败：{readError}。显示的状态可能尚未更新。</div>}
      {error && <div className="device-settings-message err">{error}</div>}
      {message && <div className="device-settings-message ok">{message}</div>}
      {!status && !readError && <p>正在读取设备恢复状态……</p>}
      {status && <>
        <h5 className="recovery-section-heading">当前生效状态</h5>
        <div className="recovery-facts recovery-overview">
          <div><span>整机卡死自动重启</span><strong>{hardwareLabel}</strong><small>{readError ? '请等待状态重新读取成功。' : hardwareDisabled ? '已按设置关闭硬件看门狗保护。' : status.hardware.enabled && status.hardware.verification_state !== 'failed' ? `停止喂狗后约 ${displayDuration(status.hardware.runtime_timeout)} 触发硬件复位` : status.hardware.summary || status.hardware.error || '尚未确认硬件保护，请查看技术信息。'}</small></div>
          <div><span>系统崩溃自动重启</span><strong>{readError ? '状态未更新' : panicTimeout == null ? '状态未知' : panicEnabled ? '已开启' : '未开启'}</strong><small>{panicEnabled ? `内核报告严重错误后${panicTimeout! < 0 ? '立即' : `约 ${panicTimeout} 秒后`}尝试重启` : '用于内核已报告严重错误、仍能执行重启代码的情况'}</small></div>
        </div>
        {status.system?.error && <div className="device-settings-message err">系统恢复设置读取失败：{status.system.error}</div>}
        {draft && <>
          <h5 className="recovery-section-heading">保护开关与时间</h5>
          <div className="system-recovery-paths">
            <fieldset className="system-recovery-path recovery-basic">
              <legend>整机卡死 · 硬件复位</legend>
              <label className="recovery-switch-row"><input type="checkbox" aria-label="整机卡死自动重启" checked={draft.policy.hardware_enabled}
                disabled={busy || !!status.system?.error || (!draft.policy.hardware_enabled && !status.system?.can_enable_hardware)}
                onChange={event => change('hardware_enabled', event.target.checked)} />
                <span><b>整机卡死自动重启</b><small>系统停止喂狗并超时后，由硬件强制复位。</small></span>
              </label>
              <div className="recovery-fields system-recovery-timeout">
                <label><span>卡死复位等待时间（秒）</span>
                  <input type="number" aria-label="卡死复位等待时间（秒）" min={1} max={86400} step={1}
                    disabled={busy || !!status.system?.error || !draft.policy.hardware_enabled || !status.system?.can_enable_hardware}
                    value={Number.isFinite(draft.policy.watchdog_timeout_sec) ? draft.policy.watchdog_timeout_sec : ''}
                    onChange={event => change('watchdog_timeout_sec', event.target.valueAsNumber)} />
                  <small>从最后一次喂狗起算，实际超时可能由硬件取整。</small>
                </label>
              </div>
              <p className="recovery-help system-recovery-effect">重启设备后生效，保存不会立即重启。</p>
            </fieldset>
            <fieldset className="system-recovery-path recovery-basic">
              <legend>系统崩溃 · 内核重启</legend>
              <label className="recovery-switch-row"><input type="checkbox" aria-label="系统崩溃自动重启" checked={draft.policy.panic_enabled}
                disabled={busy || !!status.system?.error || !status.system?.can_set_panic}
                onChange={event => change('panic_enabled', event.target.checked)} />
                <span><b>系统崩溃自动重启</b><small>内核发生严重错误（panic）后尝试软件重启。</small></span>
              </label>
              <div className="recovery-fields system-recovery-timeout">
                <label><span>崩溃重启等待时间（秒）</span>
                  <input type="number" aria-label="崩溃重启等待时间（秒）" min={0} max={2147483647} step={1}
                    disabled={busy || !!status.system?.error || !draft.policy.panic_enabled || !status.system?.can_set_panic}
                    value={Number.isFinite(draft.policy.panic_timeout_sec) ? Math.max(0, draft.policy.panic_timeout_sec) : ''}
                    onChange={event => change('panic_timeout_sec', event.target.valueAsNumber < 0 ? NaN : event.target.valueAsNumber === 0 ? -1 : event.target.valueAsNumber)} />
                  <small>从内核崩溃起算；0 表示立即重启，关闭保护请用开关。</small>
                </label>
              </div>
              <p className="recovery-help system-recovery-effect">保存后立即生效，并在重启后保留。</p>
            </fieldset>
          </div>
          <fieldset className="system-recovery-path system-recovery-reboot recovery-basic">
            <legend>重启过程卡住 · 硬件兜底</legend>
            <p className="recovery-help">随“整机卡死自动重启”启用。</p>
            <div className="recovery-fields">
              <label><span>重启阶段看门狗超时（秒）</span>
                <input type="number" aria-label="重启阶段看门狗超时（秒）" min={1} max={86400} step={1}
                  disabled={busy || !!status.system?.error || !draft.policy.hardware_enabled || !status.system?.can_enable_hardware}
                  value={Number.isFinite(draft.policy.reboot_timeout_sec) ? draft.policy.reboot_timeout_sec : ''}
                  onChange={event => change('reboot_timeout_sec', event.target.valueAsNumber)} />
                <small>保护停止服务后的最后重启阶段，不包含停止服务的耗时。</small>
              </label>
              <p className="recovery-help system-recovery-effect">重启设备后生效。{!readError && `当前实际超时：${displayDuration(status.hardware.reboot_timeout)}。`}</p>
            </div>
          </fieldset>
          <div className="device-settings-note system-recovery-relationship">
            卡死复位与崩溃重启独立计时，三个时间不相加。
          </div>
          {!status.system?.can_enable_hardware && <p className="recovery-help">硬件开关当前无法开启：{status.system?.hardware_unavailable_reason}</p>}
          {!status.system?.can_set_panic && <p className="recovery-help">无法读取内核崩溃重启状态，暂不能保存系统恢复设置，请管理员检查系统权限。</p>}
          {conflict && <div className="device-settings-note warning">系统恢复设置或实际状态已变化，编辑内容已保留。请重新读取后再保存。</div>}
          <div className="settings-actions recovery-agent-actions">
            <button className="settings-primary-btn" disabled={busy || !(dirty || needsSave) || conflict || !!readError || !!status.system?.error || !status.system?.can_set_panic} onClick={() => void save()}>{busy && !reloading ? '保存中……' : '保存系统恢复设置'}</button>
            <button className="settings-ghost-btn" disabled={busy} title="读取设备上已保存的设置、当前状态和恢复记录；未保存的修改会被放弃。" onClick={() => void reload()}>{reloading ? '读取中……' : dirty ? '放弃修改并重新读取' : '重新读取设置'}</button>
            {readAt && <span className="recovery-help recovery-read-feedback" role="status">上次手动读取：{readAt}</span>}
          </div>
        </>}
        {status.system?.hardware_pending && <div className="device-settings-note warning">硬件看门狗开关和超时设置已保存，等待正常重启设备生效。当前实际保护状态仍以上方为准，修改其他恢复设置不会取消这个待生效选择。</div>}
        {status.system?.hardware_apply_error && <div className="device-settings-message err">{status.system.hardware_apply_error}</div>}
        {status.system?.policy && !status.system.panic_in_sync && <div className="device-settings-note warning">内核当前状态与重启后使用的已保存设置不同。{dirty ? '未保存的编辑已保留，当前生效状态见上方。' : '崩溃重启开关与时间已同步内核当前状态。'}如需重启后也使用当前选择，请点击“保存系统恢复设置”；命令行临时修改不会自动写入持久配置。</div>}
        {!readError && (!hardwareDisabled || !status.system?.can_enable_hardware) && !!status.hardware.next_steps?.length && <div className="device-settings-note warning recovery-guidance" role="status">
          <b>如何处理</b>
          <ul>{status.hardware.next_steps.map(step => <li key={step}>{step}</li>)}</ul>
        </div>}
        {!readError && !status.hardware.enabled && <div className="device-settings-note warning">
          {panicEnabled ? '当前内核崩溃后仍可尝试软件重启；整机完全卡死时，尚未确认有可用的硬件复位保护。' : '当前尚未确认整机卡死保护生效。'}
          网络和程序恢复可单独配置，是否生效请查看对应页面。
        </div>}
        {status.hardware.reboot_required && <div className="device-settings-note warning">已配置下次启动使用 {status.hardware.configured_device}。当前看门狗保持运行，正常重启设备后完成切换。</div>}
        {status.hardware.enabled && status.hardware.verification_state !== 'passed' && <div className="device-settings-note warning">{status.hardware.verification_message || '尚未完成硬件复位测试'}。仅显示开启，不能证明卡死后一定能够自行重启。</div>}
        <details className="recovery-advanced">
          <summary>技术信息</summary>
          <div className="recovery-details-content">
            <div className="recovery-facts recovery-overview">
              <div><span>看门狗设备</span><strong>{status.hardware.device || '未识别'}</strong><small>{status.hardware.driver}</small></div>
              <div><span>看门狗实际超时</span><strong>{displayDuration(status.hardware.runtime_timeout)}</strong><small>重启阶段看门狗：{displayDuration(status.hardware.reboot_timeout)}</small></div>
            </div>
            {status.hardware.configured_device && <p className="recovery-help">平台配置的设备：{status.hardware.configured_device}。设备编号可能因板卡而不同，以实际驱动和接管状态为准。</p>}
            {status.hardware.diagnostics && <div className="recovery-diagnostics">
              <b>设备检查结果</b>
              {status.hardware.diagnostics.devices.length ? <ul>{status.hardware.diagnostics.devices.map(device => <li key={device.device}>
                {device.device} · 驱动：{device.driver || '未识别'} · {device.manager_owned ? '系统管理器已持有' : '系统管理器未持有'} · {device.node_present ? '设备节点存在' : '设备节点缺失'}
              </li>)}</ul> : <p>当前没有注册看门狗设备。</p>}
              {!!status.hardware.diagnostics.device_tree.length && <>
                <b>设备树</b><ul>{status.hardware.diagnostics.device_tree.map(node => <li key={node.path}>
                  {node.path} · 状态：{node.status}{node.disabled_by != null ? ` · 被关闭的节点：${node.disabled_by}` : ''}
                </li>)}</ul>
              </>}
              {!!Object.keys(status.hardware.diagnostics.kernel_config).length && <>
                <b>内核配置</b><ul>{Object.entries(status.hardware.diagnostics.kernel_config).map(([name, value]) => <li key={name}>CONFIG_{name}={value}</li>)}</ul>
                <p>驱动已编入不代表设备已启用；WATCHDOG_SYSFS 未开启只影响部分状态查询。</p>
              </>}
              {!!status.hardware.diagnostics.errors.length && <><b>未能完成的检查</b><ul>{status.hardware.diagnostics.errors.map(error => <li key={error}>{error}</li>)}</ul></>}
            </div>}
          </div>
        </details>
        <RecoveryHistory history={status.history} category="system" title="最近系统故障重启记录" />
      </>}
    </section>
  )
}
