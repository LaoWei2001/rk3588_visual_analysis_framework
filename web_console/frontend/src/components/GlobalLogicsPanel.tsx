import { useConsoleStore } from '../store/consoleStore'
import NumberField from './NumberField'
import './GlobalLogicsPanel.css'

export interface GlobalLogicEntry {
  enable: boolean
  logic: string
  channels: number[]
  poll_interval_ms: number
}

interface Props {
  logics: GlobalLogicEntry[]
  onChange: (logics: GlobalLogicEntry[]) => void
}

const DEFAULT_ENTRY: GlobalLogicEntry = {
  enable: true,
  logic: 'global_default',
  channels: [],
  poll_interval_ms: 200,
}

export default function GlobalLogicsPanel({ logics, onChange }: Props) {
  const info = useConsoleStore(s => s.info)
  const known = info?.known_global_logics ?? ['global_example', 'global_default']

  const update = (i: number, patch: Partial<GlobalLogicEntry>) => {
    const next = logics.map((l, idx) => idx === i ? { ...l, ...patch } : l)
    onChange(next)
  }

  const add = () => onChange([...logics, { ...DEFAULT_ENTRY }])
  const remove = (i: number) => onChange(logics.filter((_, idx) => idx !== i))

  return (
    <div className="gl-panel">
      <div className="gl-header">
        <span>全局逻辑配置</span>
        <button className="gl-add-btn" onClick={add}>+ 添加</button>
      </div>

      {logics.length === 0 && (
        <div className="gl-empty">无全局逻辑（可选）</div>
      )}

      {logics.map((entry, i) => (
        <div key={i} className={`gl-entry${entry.enable ? '' : ' disabled'}`}>
          <div className="gl-entry-row">
            <label className="gl-toggle">
              <input
                type="checkbox"
                checked={entry.enable}
                onChange={e => update(i, { enable: e.target.checked })}
              />
              启用
            </label>

            <div className="gl-field" style={{ flex: 2 }}>
              <label>逻辑名称</label>
              <select
                value={entry.logic}
                onChange={e => update(i, { logic: e.target.value })}
              >
                {known.map(l => <option key={l} value={l}>{l}</option>)}
                {!known.includes(entry.logic) && (
                  <option value={entry.logic}>{entry.logic}</option>
                )}
              </select>
            </div>

            <div className="gl-field" style={{ flex: 2 }}>
              <label>监控通道（空=全部）</label>
              <input
                value={entry.channels.join(', ')}
                placeholder="0, 1, 2"
                onChange={e => {
                  const ids = e.target.value
                    .split(',')
                    .map(s => s.trim())
                    .filter(Boolean)
                    .map(Number)
                    .filter(n => !isNaN(n))
                  update(i, { channels: ids })
                }}
              />
            </div>

            <div className="gl-field" style={{ flex: 1 }}>
              <label>轮询间隔 (ms)</label>
              <NumberField
                def={200}
                value={entry.poll_interval_ms}
                onChange={v => update(i, { poll_interval_ms: v ?? 200 })}
              />
            </div>

            <button className="gl-remove-btn" onClick={() => remove(i)} title="删除">✕</button>
          </div>
        </div>
      ))}
    </div>
  )
}
