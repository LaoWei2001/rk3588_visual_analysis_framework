import './ConfigSyncNotice.css'

export default function ConfigSyncNotice({ error = '', conflict = false, onReload }:
  { error?: string; conflict?: boolean; onReload?: () => void }) {
  if (!error && !conflict) return null
  return <div className="config-sync-notice" role="status">
    {error && <span>自动同步失败：{error}。当前显示可能已过期，连接恢复后将自动重试。</span>}
    {conflict && <span>底层配置已变化，未保存的修改已保留。请重新同步后再保存。
      {onReload && <button type="button" onClick={onReload}>放弃修改并同步</button>}</span>}
  </div>
}
