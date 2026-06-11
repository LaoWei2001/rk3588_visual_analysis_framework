import { useState } from 'react'

interface AssetPickerProps {
  value: string
  onChange: (v: string) => void
  options: string[]
  placeholder?: string
}

/**
 * 如果有可选文件列表 → 默认显示下拉框（带"✏手动输入"切换按钮）
 * 如果没有文件列表   → 直接显示文本输入框
 * 如果当前值不在列表中 → 自动切到手动输入模式
 */
export default function AssetPicker({ value, onChange, options, placeholder }: AssetPickerProps) {
  const [editMode, setEditMode] = useState(
    () => options.length === 0 || (value !== '' && !options.includes(value))
  )

  if (editMode) {
    return (
      <div className="asset-picker">
        <input
          value={value}
          onChange={e => onChange(e.target.value)}
          placeholder={placeholder}
        />
        {options.length > 0 && (
          <button
            type="button"
            className="picker-toggle"
            onClick={() => setEditMode(false)}
            title="从 assets 列表选择"
          >
            📂
          </button>
        )}
      </div>
    )
  }

  return (
    <div className="asset-picker">
      <select value={value} onChange={e => onChange(e.target.value)}>
        <option value="">— 选择文件 —</option>
        {options.map(o => (
          <option key={o} value={o}>{o}</option>
        ))}
      </select>
      <button
        type="button"
        className="picker-toggle"
        onClick={() => setEditMode(true)}
        title="手动输入路径"
      >
        ✏
      </button>
    </div>
  )
}
