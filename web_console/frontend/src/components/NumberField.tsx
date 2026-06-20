import { useState, useEffect, type CSSProperties } from 'react'

export interface NumberFieldProps {
  /** 当前值（数字 / undefined / null 都可，节点数据常是 unknown）。 */
  value: unknown
  /** 输入合法数字时回调；allowEmpty 时清空回调 undefined。 */
  onChange: (v: number | undefined) => void
  /** 允许留空：留空向上回报 undefined（表示“用默认 / 全局”），失焦保持空。 */
  allowEmpty?: boolean
  /** 不允许留空时，失焦若为空 / 非法则回填到此默认值。 */
  def?: number
  min?: number | string
  max?: number | string
  step?: number | string
  placeholder?: string
  className?: string
  style?: CSSProperties
}

/**
 * 受控数字输入框 —— 用本地字符串状态，允许整段清空、允许小数中间态（如 "0."），
 * 不会因为删空就跳回 0 / 默认值。
 *
 * 背景：各处手写的 `value={Number(d.x ?? def)}` + `onChange={+e.target.value}` 有通病——
 * 用户清空输入框时 `+"" === 0`（或回退到默认值），导致数字“删不掉”、一删就跳回。
 * 本组件把它修好并统一：
 *  - 只把“合法数字”回报父级；空串按 allowEmpty 决定回报 undefined 还是不回报（父级保留原值）。
 *  - 外部 value 变化（加载配置 / 切换节点 / 切 logic 填默认）时同步显示，但不会把用户正在输入的 "0." 改写成 "0"。
 *  - allowEmpty=false（默认）：失焦若为空则回填到 def 或上一个合法值（字段“总有值”）。
 *    allowEmpty=true：失焦保持空并回报 undefined（字段“可留空 = 用默认 / 全局”，如 tracker 覆盖项）。
 */
export default function NumberField({
  value, onChange, allowEmpty = false, def,
  min, max, step, placeholder, className, style,
}: NumberFieldProps) {
  const [text, setText] = useState(value == null ? '' : String(value))

  // 仅当外部值与当前文本解析结果不一致时同步，避免把用户正在输入的 "0." 改写成 "0"
  useEffect(() => {
    if (Number(text) !== Number(value)) setText(value == null ? '' : String(value))
  }, [value]) // eslint-disable-line react-hooks/exhaustive-deps

  return (
    <input
      type="number"
      className={className}
      style={style}
      step={step}
      min={min}
      max={max}
      placeholder={placeholder}
      value={text}
      onChange={e => {
        const t = e.target.value
        setText(t)
        if (t === '') { if (allowEmpty) onChange(undefined) }
        else if (!isNaN(Number(t))) onChange(Number(t))
      }}
      onBlur={() => {
        if (text === '' || isNaN(Number(text))) {
          if (allowEmpty) { setText(''); onChange(undefined) }
          else setText(value != null ? String(value) : (def != null ? String(def) : ''))
        }
      }}
    />
  )
}
