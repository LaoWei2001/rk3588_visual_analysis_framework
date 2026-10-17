import { useEffect, useState } from 'react'
import { useAutoSync, useSyncedDraft } from '../../hooks/useAutoSync'
import ConfigSyncNotice from '../../components/ConfigSyncNotice'
import {
  cleanupRootStorageTargets,
  cleanupStorageNow,
  fetchStorageSettings,
  saveStorageSettings,
  type StorageSettings,
} from '../../api/client'

const err = (e: unknown) => (e as { response?: { data?: { detail?: string } } })?.response?.data?.detail || (e instanceof Error ? e.message : String(e))
const bytes = (value: number) => {
  if (value < 1024) return `${value} B`
  const units = ['KB', 'MB', 'GB', 'TB']
  let size = value / 1024; let unit = 0
  while (size >= 1024 && unit < units.length - 1) { size /= 1024; unit++ }
  return `${size.toFixed(size >= 10 ? 1 : 2)} ${units[unit]}`
}

export default function StorageSettingsSection() {
  const [data, setData] = useState<StorageSettings | null>(null)
  const policyOf = (value: StorageSettings) => ({ auto_cleanup: value.auto_cleanup, retention_days: value.retention_days, max_event_store_gb: value.max_event_store_gb, min_free_gb: value.min_free_gb })
  const [draft, setDraft, draftSync] = useSyncedDraft(data ? policyOf(data) : null, { auto_cleanup: false, retention_days: 30, max_event_store_gb: 1, min_free_gb: 1 })
  const [selectedTargets, setSelectedTargets] = useState<string[]>([])
  const [busy, setBusy] = useState<'save' | 'cleanup' | 'root-cleanup' | 'refresh' | null>(null)
  const [message, setMessage] = useState<{ text: string; type: 'ok' | 'err' } | null>(null)

  const apply = (value: StorageSettings) => {
    setData(value)
    setSelectedTargets(current => current.filter(key => (
      value.root_cleanup_targets.some(target => target.key === key && target.exists)
    )))
  }
  const load = async () => {
    setBusy('refresh')
    try { apply(await fetchStorageSettings()) }
    catch (e) { setMessage({ text: `读取存储信息失败：${err(e)}`, type: 'err' }) }
    finally { setBusy(null) }
  }
  useEffect(() => { load() }, [])
  const syncError = useAutoSync(() => fetchStorageSettings(data === null), value => {
    setData(current => current ? { ...current, ...value } : value)
    setMessage(current => current?.text.startsWith('读取存储信息失败：') ? null : current)
  }, { enabled: busy === null, immediate: false })

  const save = async () => {
    setBusy('save'); setMessage(null)
    try {
      const latest = await fetchStorageSettings(false)
      setData(current => current ? { ...current, ...latest } : current)
      if (!draftSync.isCurrent(policyOf(latest))) throw new Error('底层存储策略已变化，请重新同步后再保存')
      const value = await saveStorageSettings(draft)
      apply(value); draftSync.accept(policyOf(value))
      setMessage({ text: '存储策略已保存', type: 'ok' })
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }
  const cleanup = async () => {
    if (!window.confirm('立即按当前保留天数、容量上限和最小剩余空间执行清理？只会删除各视觉程序生成的告警记录（包括图片和录像）。')) return
    setBusy('cleanup'); setMessage(null)
    try {
      const value = await cleanupStorageNow(); apply(value)
      setMessage({ text: `清理完成：删除 ${value.cleanup.deleted_count} 条告警记录，释放 ${bytes(value.cleanup.deleted_bytes)}`, type: 'ok' })
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }

  const cleanupRoot = async () => {
    if (!data || selectedTargets.length === 0) return
    const selected = data.root_cleanup_targets.filter(target => selectedTargets.includes(target.key))
    const paths = selected.map(target => target.path).join('\n')
    if (!window.confirm(`确定永久删除以下目录或文件？此操作不可恢复：\n\n${paths}\n\n/root/.nvm 不会被清理。`)) return
    setBusy('root-cleanup'); setMessage(null)
    try {
      const value = await cleanupRootStorageTargets(selectedTargets)
      apply(value)
      setSelectedTargets([])
      const result = value.root_cleanup
      if (result.errors.length) {
        setMessage({ text: `已清理 ${result.deleted_count} 项，预计释放 ${bytes(result.freed_bytes)}；另有 ${result.errors.length} 项清理失败`, type: 'err' })
      } else {
        setMessage({ text: `开发工具与缓存清理完成：删除 ${result.deleted_count} 项，预计释放 ${bytes(result.freed_bytes)}`, type: 'ok' })
      }
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }

  const availableTargets = data?.root_cleanup_targets.filter(target => target.exists) || []
  const selectedBytes = availableTargets
    .filter(target => selectedTargets.includes(target.key))
    .reduce((total, target) => total + target.bytes, 0)

  const level = (data?.used_percent || 0) >= 90 ? 'danger' : (data?.used_percent || 0) >= 80 ? 'warn' : 'good'
  return (
    <div className="settings-section">
      <div className="settings-section-title"><h3>存储</h3></div>
      {message && <div className={`device-settings-message ${message.type}`}>{message.text}</div>}
      <ConfigSyncNotice error={syncError} conflict={draftSync.conflict} onReload={draftSync.reset} />
      {!data ? <div className="device-settings-state">正在读取存储信息……</div> : <>
        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>磁盘空间</h4><p>{data.storage_path}</p></div><button className="settings-ghost-btn" disabled={busy !== null} onClick={load}>刷新</button></div>
          <div className="storage-metrics">
            <div><span>总容量</span><b>{bytes(data.total_bytes)}</b></div><div><span>已使用</span><b>{bytes(data.used_bytes)}</b></div><div><span>可用</span><b>{bytes(data.free_bytes)}</b></div><div><span>告警记录</span><b>{bytes(data.event_bytes)} / {data.event_count} 条</b></div>
          </div>
          <div className="storage-bar"><span className={level} style={{ width: `${Math.min(100, data.used_percent)}%` }} /></div>
          <p className={`storage-usage-text ${level}`}>磁盘已使用 {data.used_percent}%{data.used_percent >= 85 ? '，建议尽快释放空间' : ''}</p>
        </section>

        <section className="device-settings-card root-cleanup-card">
          <div className="device-settings-card-head">
            <div><h4>Root 开发工具与缓存清理</h4></div>
            <b className="root-cleanup-total">可清理 {bytes(availableTargets.reduce((total, target) => total + target.bytes, 0))}</b>
          </div>
          <div className="root-cleanup-list">
            {data.root_cleanup_targets.map(target => (
              <label key={target.key} className={!target.exists ? 'unavailable' : ''}>
                <input
                  type="checkbox"
                  checked={selectedTargets.includes(target.key)}
                  disabled={!target.exists || busy !== null}
                  onChange={e => setSelectedTargets(current => e.target.checked
                    ? [...current, target.key]
                    : current.filter(key => key !== target.key))}
                />
                <span className="root-cleanup-check" />
                <span className="root-cleanup-detail">
                  <b>{target.label}</b>
                  <code>{target.path}</code>
                  <small>{target.description}</small>
                </span>
                <strong>{target.exists ? bytes(target.bytes) : '不存在'}</strong>
              </label>
            ))}
          </div>
          <div className="device-settings-note warning">所选目录将永久删除，缓存可能重新下载；保留 Node 环境。</div>
          <div className="root-cleanup-footer">
            <div>
              <button className="settings-text-btn" disabled={busy !== null || availableTargets.length === 0} onClick={() => setSelectedTargets(availableTargets.map(target => target.key))}>全选现有项</button>
              <button className="settings-text-btn" disabled={busy !== null || selectedTargets.length === 0} onClick={() => setSelectedTargets([])}>清空选择</button>
            </div>
            <span>已选择 {selectedTargets.length} 项，约 {bytes(selectedBytes)}</span>
            <button className="settings-ghost-btn danger" disabled={busy !== null || selectedTargets.length === 0} onClick={cleanupRoot}>{busy === 'root-cleanup' ? '清理中……' : '清理所选目录'}</button>
          </div>
        </section>

        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>告警记录保留策略</h4><p>仅清理告警记录，保留程序、模型和配置。</p></div>
            <label className="system-switch"><input type="checkbox" checked={draft.auto_cleanup} disabled={busy !== null} onChange={e => setDraft(v => ({ ...v, auto_cleanup: e.target.checked }))} /><span /><b>{draft.auto_cleanup ? '自动清理' : '已关闭'}</b></label>
          </div>
          <div className="storage-policy-grid">
            <label><span>记录保留天数</span><input type="number" min="1" max="3650" value={draft.retention_days} disabled={busy !== null} onChange={e => setDraft(v => ({ ...v, retention_days: Number(e.target.value) }))} /></label>
            <label><span>单程序记录容量上限（GB）</span><input type="number" min="0.1" max="1000" step="0.1" value={draft.max_event_store_gb} disabled={busy !== null} onChange={e => setDraft(v => ({ ...v, max_event_store_gb: Number(e.target.value) }))} /></label>
            <label><span>磁盘最小保留空间（GB）</span><input type="number" min="0" max="1000" step="0.1" value={draft.min_free_gb} disabled={busy !== null} onChange={e => setDraft(v => ({ ...v, min_free_gb: Number(e.target.value) }))} /></label>
          </div>
          <div className="device-settings-note">达到保留限制时优先回收旧记录，跳过正在写入或上传的记录；容量与空间限制下次启动程序生效。</div>
          <div className="settings-actions"><button className="settings-ghost-btn danger" disabled={busy !== null} onClick={cleanup}>{busy === 'cleanup' ? '清理中……' : '按记录策略清理'}</button><button className="settings-primary-btn" disabled={busy !== null || draftSync.conflict || !!syncError} onClick={save}>{busy === 'save' ? '保存中……' : '保存存储策略'}</button></div>
        </section>
      </>}
    </div>
  )
}
