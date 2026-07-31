import { useEffect, useState } from 'react'
import { asLogicDef, fetchAppLogics, type LogicDef, type LogicParam } from '../api/client'
import { useEditorStore } from '../store/editorStore'
import NumberField from './NumberField'
import './GlobalLogicsPanel.css'

export interface GlobalLogicEntry {
  enable: boolean
  logic: string
  channels: number[]
  poll_interval_ms: number
  logic_parameters: Record<string, unknown>
}

interface Props {
  logics: GlobalLogicEntry[]
  onChange: (logics: GlobalLogicEntry[]) => void
}

export default function GlobalLogicsPanel({ logics, onChange }: Props) {
  const appName = useEditorStore(s => s.appName)
  const [definitions, setDefinitions] = useState<LogicDef[]>([])
  const [catalogError, setCatalogError] = useState('')

  useEffect(() => {
    let cancelled = false
    setDefinitions([])
    setCatalogError('')
    if (!appName) return () => { cancelled = true }
    fetchAppLogics(appName)
      .then(result => {
        if (!cancelled) setDefinitions(result.global_logics.map(asLogicDef))
      })
      .catch(() => {
        if (!cancelled) setCatalogError('无法读取当前应用的全局逻辑清单')
      })
    return () => { cancelled = true }
  }, [appName])

  const update = (i: number, patch: Partial<GlobalLogicEntry>) => {
    const next = logics.map((l, idx) => idx === i ? { ...l, ...patch } : l)
    onChange(next)
  }

  const defaultsFor = (definition?: LogicDef): Record<string, unknown> => {
    const parameters: Record<string, unknown> = {}
    ;(definition?.params ?? []).forEach(param => {
      if (param.default !== undefined) parameters[param.key] = param.default
    })
    return parameters
  }
  const add = () => {
    const definition = definitions[0]
    onChange([...logics, {
      enable: true,
      logic: definition?.name ?? '',
      channels: [],
      poll_interval_ms: 200,
      logic_parameters: defaultsFor(definition),
    }])
  }
  const remove = (i: number) => onChange(logics.filter((_, idx) => idx !== i))

  return (
    <div className="gl-panel">
      <div className="gl-header">
        <span>全局逻辑配置</span>
        <button className="gl-add-btn" onClick={add} disabled={definitions.length === 0}>+ 添加</button>
      </div>

      {logics.length === 0 && (
        <div className="gl-empty">无全局逻辑（可选）</div>
      )}
      {catalogError && <div className="gl-empty">⚠ {catalogError}</div>}
      {!catalogError && definitions.length === 0 && appName && (
        <div className="gl-empty">当前应用没有声明全局逻辑模块。</div>
      )}

      {logics.map((entry, i) => {
        const definition = definitions.find(item => item.name === entry.logic)
        const names = definitions.map(item => item.name)
        const moduleParameters = entry.logic_parameters ?? {}
        return (
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
                onChange={e => {
                  const selected = definitions.find(item => item.name === e.target.value)
                  update(i, {
                    logic: e.target.value,
                    logic_parameters: defaultsFor(selected),
                  })
                }}
              >
                {definitions.map(item => (
                  <option key={item.name} value={item.name}>
                    {item.label ? `${item.label}（${item.name}）` : item.name}
                  </option>
                ))}
                {entry.logic && !names.includes(entry.logic) && (
                  <option value={entry.logic}>{entry.logic}（⚠ 不在当前应用清单）</option>
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
          {(definition?.params ?? []).length > 0 && (
            <div className="gl-params">
              {(definition?.params ?? []).map(param => (
                <GlobalParamField
                  key={param.key}
                  param={param}
                  value={moduleParameters[param.key]}
                  onChange={value => update(i, {
                    logic_parameters: { ...moduleParameters, [param.key]: value },
                  })}
                />
              ))}
            </div>
          )}
          </div>
        )
      })}
    </div>
  )
}

function GlobalParamField({ param, value, onChange }: {
  param: LogicParam
  value: unknown
  onChange: (value: unknown) => void
}) {
  const label = `${param.label ?? param.key}${param.unit ? `（${param.unit}）` : ''}`
  if (param.type === 'bool') {
    return (
      <label className="gl-toggle">
        <input type="checkbox"
          checked={value !== undefined ? !!value : !!param.default}
          onChange={event => onChange(event.target.checked)} />
        {label}
      </label>
    )
  }
  if (param.type === 'enum') {
    return (
      <div className="gl-field">
        <label>{label}</label>
        <select value={String(value ?? param.default ?? '')}
          onChange={event => onChange(event.target.value)}>
          {(param.options ?? []).map(option => (
            <option key={option} value={option}>{option}</option>
          ))}
        </select>
      </div>
    )
  }
  if (param.type === 'json') {
    return <GlobalJsonParamField param={param} value={value} onChange={onChange} />
  }
  if (param.type === 'int' || param.type === 'float') {
    return (
      <div className="gl-field">
        <label>{label}</label>
        <NumberField
          value={value ?? param.default}
          def={param.default !== undefined ? Number(param.default) : undefined}
          min={param.min} max={param.max}
          step={param.step ?? (param.type === 'float' ? 0.01 : 1)}
          integerOnly={param.type === 'int'}
          onChange={onChange} />
      </div>
    )
  }
  if (param.type === 'text') {
    return (
      <div className="gl-field">
        <label>{label}</label>
        <textarea rows={3} value={String(value ?? param.default ?? '')}
          placeholder={param.placeholder}
          onChange={event => onChange(event.target.value)} />
      </div>
    )
  }
  return (
    <div className="gl-field">
      <label>{label}</label>
      <input value={String(value ?? param.default ?? '')}
        placeholder={param.placeholder}
        onChange={event => onChange(event.target.value)} />
    </div>
  )
}

function GlobalJsonParamField({ param, value, onChange }: {
  param: LogicParam
  value: unknown
  onChange: (value: unknown) => void
}) {
  const render = (input: unknown) => JSON.stringify(input ?? param.default ?? null, null, 2)
  const [text, setText] = useState(render(value))
  const [invalid, setInvalid] = useState(false)
  useEffect(() => setText(render(value)), [value]) // eslint-disable-line react-hooks/exhaustive-deps

  return (
    <div className="gl-field gl-json-param">
      <label>{param.label ?? param.key}</label>
      <textarea rows={4} value={text} onChange={event => {
        const next = event.target.value
        setText(next)
        try {
          const parsed = JSON.parse(next)
          const matches = param.json_type === 'array'
            ? Array.isArray(parsed)
            : param.json_type === 'object'
              ? parsed !== null && typeof parsed === 'object' && !Array.isArray(parsed)
              : true
          setInvalid(!matches)
          if (matches) onChange(parsed)
        } catch {
          setInvalid(true)
        }
      }} />
      {invalid && <span className="gl-param-error">JSON 格式或容器类型不正确</span>}
    </div>
  )
}
