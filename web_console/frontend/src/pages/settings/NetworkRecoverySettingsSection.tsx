import { useEffect, useRef, useState } from 'react'
import { fetchNetworkRecovery, saveNetworkRecovery, type NetworkRecoveryPolicy, type NetworkRecoveryStatus, type NetworkRecoveryTarget } from '../../api/client'
import './RecoverySettingsSection.css'
import RecoveryHistory from './RecoveryHistory'
import { readSuccessMessage, readTimestamp } from './recoveryFeedback'

type Draft = { policy: NetworkRecoveryPolicy; original: NetworkRecoveryPolicy; revision: string }
const draftOf = (status: NetworkRecoveryStatus): Draft => ({
  policy: { ...status.policy, targets: status.policy.targets.map(target => ({ ...target })) },
  original: status.policy, revision: status.revision,
})
const messageOf = (error: unknown) => {
  const detail = (error as { response?: { data?: { detail?: unknown } } })?.response?.data?.detail
  return typeof detail === 'string' ? detail : error instanceof Error ? error.message : String(error)
}
const withMethod = (target: NetworkRecoveryTarget, method: string): NetworkRecoveryTarget => method === 'ping'
  ? { host: target.host, method: 'ping' }
  : { host: target.host, port: target.method === 'ping' ? 443 : target.port }
const fields = [
  ['failure_timeout_sec', '持续异常确认（秒）', 60, 1800],
  ['recovery_wait_sec', '每级恢复后等待（秒）', 30, 600],
  ['action_cooldown_sec', '网络操作最小间隔（秒）', 60, 3600],
  ['reboot_after_sec', '持续异常整机重启期限（秒）', 300, 7200],
  ['reboot_cooldown_sec', '整机重启最小间隔（秒）', 1800, 86400],
  ['max_reboots_per_day', '24 小时最多网络故障重启次数', 1, 5],
] as const

export default function NetworkRecoverySettingsSection() {
  const [status, setStatus] = useState<NetworkRecoveryStatus | null>(null)
  const [draft, setDraft] = useState<Draft | null>(null)
  const [busy, setBusy] = useState(false)
  const [reloading, setReloading] = useState(false)
  const [readAt, setReadAt] = useState('')
  const [error, setError] = useState('')
  const [readError, setReadError] = useState('')
  const [message, setMessage] = useState('')
  const advanced = useRef<HTMLDetailsElement>(null)
  const generation = useRef(0)
  const operating = useRef(false)
  const dirty = !!draft && JSON.stringify(draft.policy) !== JSON.stringify(draft.original)
  const conflict = !!draft && !!status && draft.revision !== status.revision

  useEffect(() => {
    let active = true
    let pending = false
    const refresh = async () => {
      if (pending || operating.current) return
      pending = true
      const requestGeneration = generation.current
      try {
        const value = await fetchNetworkRecovery()
        if (active && generation.current === requestGeneration) { setStatus(value); setDraft(current => current && JSON.stringify(current.policy) !== JSON.stringify(current.original) ? current : draftOf(value)); setReadError('') }
      } catch (failure) { if (active && generation.current === requestGeneration) setReadError(messageOf(failure)) }
      finally { pending = false }
    }
    void refresh()
    const timer = window.setInterval(refresh, 1000)
    return () => { active = false; window.clearInterval(timer) }
  }, [])

  const change = (key: keyof NetworkRecoveryPolicy, value: NetworkRecoveryPolicy[keyof NetworkRecoveryPolicy]) => {
    if (draft) { setDraft({ ...draft, policy: { ...draft.policy, [key]: value } }); setMessage(''); setError('') }
  }
  const act = async (policy?: NetworkRecoveryPolicy) => {
    if (!draft) return
    generation.current += 1
    operating.current = true
    setBusy(true); setReloading(!policy); setError(''); setMessage('')
    try {
      const value = policy ? await saveNetworkRecovery(policy, draft.revision) : await fetchNetworkRecovery()
      setStatus(value); setDraft(draftOf(value)); setReadError('')
      if (policy) setMessage('网络恢复策略已保存，独立守护将在下一轮巡检时使用。')
      else if (!value.policy_error) { const time = readTimestamp(); setReadAt(time); setMessage(readSuccessMessage(dirty, time)) }
    } catch (failure) { setError(messageOf(failure)); if (policy && advanced.current) advanced.current.open = true }
    finally { generation.current += 1; operating.current = false; setBusy(false); setReloading(false) }
  }
  const save = () => {
    if (!draft || conflict || readError) return
    for (const [key, label, min, max] of fields) {
      const value = draft.policy[key]
      if (!Number.isInteger(value) || value < min || value > max) {
        setError(`${label}必须为 ${min}～${max} 的整数`)
        if (advanced.current) advanced.current.open = true
        return
      }
    }
    if (draft.policy.reboot_after_sec < draft.policy.failure_timeout_sec + 2 * draft.policy.recovery_wait_sec) {
      setError('整机重启期限至少应包含异常确认和两次恢复等待时间')
      if (advanced.current) advanced.current.open = true
      return
    }
    const targets = draft.policy.targets.map(t => ({ ...t, host: t.host.trim() }))
    if (!draft.policy.enabled && targets.length === 1 && !targets[0].host) targets.pop()
    if (draft.policy.enabled && !targets.length) { setError('请填写网络检测目标地址'); return }
    const invalidTarget = targets.findIndex(t => !t.host || (t.method !== 'ping' && (!Number.isInteger(t.port) || t.port < 1 || t.port > 65535)))
    if (invalidTarget !== -1) {
      setError('请填写检测目标地址；使用 TCP 检测时，端口必须为 1～65535 的整数')
      if (invalidTarget > 0 && advanced.current) advanced.current.open = true
      return
    }
    void act({ ...draft.policy, targets })
  }
  const observation = status?.runtime?.observation
  const primary: NetworkRecoveryTarget = draft?.policy.targets[0] ?? { host: '', method: 'ping' }
  const changeTarget = (index: number, value: NetworkRecoveryTarget) => {
    if (!draft) return
    const targets = [...draft.policy.targets]
    targets[index] = value
    change('targets', targets)
  }

  return <section className="device-settings-card recovery-settings">
    <div className="device-settings-card-head"><div><h4>网络故障恢复</h4></div></div>
    {(error || readError) && <div className="device-settings-message err">{error || readError}。显示状态可能尚未更新。</div>}
    {message && <div className="device-settings-message ok">{message}</div>}
    {!status && !readError && <p>正在读取网络恢复状态……</p>}
    {status && <>
      <div className="recovery-facts recovery-overview">
        <div><span>网络恢复服务</span><strong>{!status.service.installed ? '未安装' : status.requirements?.ready === false ? '暂不可用' : !status.service.active ? '未运行' : !status.service.heartbeat_fresh ? '等待巡检' : status.policy.enabled ? '已开启' : '未开启'}</strong><small>{observation?.interface ? `当前网卡：${observation.interface}` : '支持有线与 Wi-Fi'}</small></div>
        <div><span>网络状态</span><strong>{status.policy.enabled ? status.runtime?.message || '等待检测' : '开启后自动检测'}</strong>{status.policy.enabled && !status.runtime?.armed && <small>先成功检测一次目标，再启动自动恢复</small>}</div>
      </div>
      {!status.service.installed && <div className="device-settings-note warning">请使用项目安装器安装独立网络恢复服务。</div>}
      {status.service.installed && status.requirements?.message && <div className="device-settings-note warning">{status.requirements.message}</div>}
      {status.service.installed && !status.service.active && <div className="device-settings-note warning">网络恢复服务已停止，保存策略后也无法执行自动恢复。请检查设备上的网络恢复服务。</div>}
      {status.policy_error && <div className="device-settings-message err">策略文件异常：{status.policy_error}</div>}
      {status.interface_error && <div className="device-settings-note warning">网卡列表读取失败：{status.interface_error}</div>}
      {status.runtime?.error && <div className="device-settings-message err">{status.runtime.error}</div>}
      {status.policy_pending && <div className="device-settings-note">已保存的策略等待下一轮巡检生效。</div>}
      {draft && <>
        <div className="recovery-switches recovery-basic">
          <label className="recovery-switch-row"><input type="checkbox" checked={draft.policy.enabled} disabled={busy || (!draft.policy.enabled && status.requirements?.ready === false)} onChange={e => change('enabled', e.target.checked)} /><span><b>网络失联自动恢复</b><small>持续失联时自动重连；仅目标设备故障或有线未接线时，不因此重启盒子。</small></span></label>
        </div>
        <div className={`recovery-fields network-recovery-primary${primary.method !== 'ping' ? ' network-recovery-primary-tcp' : ''}`}>
          <label><span>网络检测目标地址</span><input placeholder="设备 IP 或域名，不含 http://" value={primary.host} disabled={busy} onChange={e => changeTarget(0, { ...primary, host: e.target.value })} /></label>
          <label><span>检测方式</span><select aria-label="检测方式" value={primary.method ?? 'tcp'} disabled={busy} onChange={e => changeTarget(0, withMethod(primary, e.target.value))}>
            <option value="ping">仅检测设备（ping）</option><option value="tcp">检测服务端口（TCP）</option>
          </select></label>
          {primary.method !== 'ping' && <label><span>服务端口</span><input type="number" min={1} max={65535} value={primary.port} disabled={busy} onChange={e => changeTarget(0, { ...primary, port: Number(e.target.value) })} /></label>}
        </div>
        <p className="recovery-help">{primary.method === 'ping' ? '目标需保持在线并允许 ping。' : '填写目标开放的端口，如 443。'}</p>
        <div className="recovery-switches">
          <label className="recovery-switch-row"><input type="checkbox" checked={draft.policy.allow_reboot} disabled={busy} onChange={e => change('allow_reboot', e.target.checked)} /><span><b>必要时允许重启盒子</b><small>本机网络持续异常且重连无效时，按限制重启。</small></span></label>
        </div>
        <details className="recovery-advanced" ref={advanced}>
          <summary>高级设置</summary>
          <div className="recovery-details-content">
            <div className="recovery-fields"><label><span>远程管理网卡</span><select value={draft.policy.interface} disabled={busy} onChange={e => change('interface', e.target.value)}>
              <option value="">自动跟随默认路由（有线 / Wi-Fi）</option>
              {status.interfaces.map(i => <option key={i.name} value={i.name} disabled={i.state === 'unmanaged'}>{i.name} · {i.type === 'wifi' ? 'Wi-Fi' : '有线'}{i.state === 'unmanaged' ? '（未由系统网络管理接管）' : ''}</option>)}
              {draft.policy.interface && !status.interfaces.some(i => i.name === draft.policy.interface) && <option value={draft.policy.interface}>{draft.policy.interface}（当前未识别）</option>}
            </select></label></div>
            <div className="device-settings-note">选择管理网卡，避免使用摄像头专用网口。</div>
            <div className="network-recovery-targets"><b>备用检测目标（可选）</b><p>包含主要目标在内最多 4 个；任意一个可达即正常。</p>
              {draft.policy.targets.slice(1).map((target, offset) => { const index = offset + 1; return <div className={`network-recovery-target${target.method !== 'ping' ? ' network-recovery-target-tcp' : ''}`} key={index}>
                <input aria-label={`备用目标地址 ${index}`} placeholder="设备 IP 或域名" value={target.host} disabled={busy} onChange={e => changeTarget(index, { ...target, host: e.target.value })} />
                <select aria-label={`备用检测方式 ${index}`} value={target.method ?? 'tcp'} disabled={busy} onChange={e => changeTarget(index, withMethod(target, e.target.value))}>
                  <option value="ping">仅检测设备（ping）</option><option value="tcp">检测服务端口（TCP）</option>
                </select>
                {target.method !== 'ping' && <input aria-label={`备用服务端口 ${index}`} type="number" min={1} max={65535} value={target.port} disabled={busy} onChange={e => changeTarget(index, { ...target, port: Number(e.target.value) })} />}
                <button className="settings-ghost-btn" disabled={busy} onClick={() => change('targets', draft.policy.targets.filter((_, i) => i !== index))}>移除</button>
              </div> })}
              <button className="settings-ghost-btn" disabled={busy || !draft.policy.targets[0]?.host.trim() || draft.policy.targets.length >= 4} onClick={() => change('targets', [...draft.policy.targets, { host: '', method: 'ping' }])}>添加备用目标</button>
            </div>
            <div className="recovery-switches">
              <label className="recovery-switch-row"><input type="checkbox" checked={draft.policy.allow_manager_restart} disabled={busy} onChange={e => change('allow_manager_restart', e.target.checked)} /><span><b>连接恢复失败后允许重启网络管理服务</b><small>可能短暂影响其他网口。</small></span></label>
            </div>
            <div className="recovery-fields">{fields.map(([key, label, min, max]) => <label key={key}><span>{label}</span><input type="number" min={min} max={max} value={draft.policy[key]} disabled={busy} onChange={e => change(key, Number(e.target.value))} /></label>)}</div>
            <div className="device-settings-note">网络修复最多 6 次/小时；故障重启共享额度，取较长间隔、较少次数。</div>
            <div className="recovery-facts recovery-overview">
              <div><span>当前管理网卡</span><strong>{observation?.interface || '等待识别'}</strong><small>{observation?.addresses.join(' / ')}</small><small>网关：{observation?.gateway || '未识别'} · {observation?.gateway_reachable == null ? '未探测' : observation.gateway_reachable ? '可达' : '未连通'}</small></div>
              <div><span>首次成功检测</span><strong>{status.runtime?.armed ? '已确认' : '等待首次成功'}</strong>{!!status.runtime?.fault_age_sec && <small>已持续异常 {status.runtime.fault_age_sec} 秒</small>}</div>
            </div>
            {!!observation?.targets.length && <div className="network-recovery-probes">{observation.targets.map((t, index) => <span key={index}>{t.host}{t.method === 'ping' ? '（ping）' : `:${t.port}（TCP）`} · {t.reachable ? '可达' : '未连通'}</span>)}</div>}
            <div className="settings-actions recovery-agent-actions"><button className="settings-ghost-btn" disabled={busy} title="读取设备上已保存的策略、当前状态和恢复记录；未保存的修改会被放弃。" onClick={() => void act()}>{reloading ? '读取中……' : dirty ? '放弃修改并重新读取' : '重新读取策略'}</button>{readAt && <span className="recovery-help recovery-read-feedback" role="status">上次手动读取：{readAt}</span>}</div>
          </div>
        </details>
        {conflict && <div className="device-settings-note warning">底层网络恢复策略已变化，未保存修改已保留。请重新读取策略后再保存。</div>}
        <div className="settings-actions recovery-agent-actions"><button className="settings-primary-btn" disabled={busy || !dirty || conflict || !!readError || !status.service.installed || !!status.policy_error} onClick={save}>{busy && !reloading ? '处理中……' : '保存网络恢复策略'}</button></div>
      </>}
      <RecoveryHistory history={status.history} category="network" title="最近网络恢复记录" />
    </>}
  </section>
}
