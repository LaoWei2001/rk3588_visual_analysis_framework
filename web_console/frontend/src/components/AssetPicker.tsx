import { useRef } from 'react'

interface AssetPickerProps {
  value: string
  onChange: (v: string) => void
  options: string[]
  emptyHint?: string                 // options 为空时显示的提示（取代旧的手动输入框）
  accept?: string                    // 导入按钮的文件类型过滤（如 ".rknn"）
  uploading?: boolean                // 上传中：禁用导入按钮并显示 …
  onUpload?: (file: File) => void    // 提供则显示「导入」按钮
}

/**
 * 资源选择器：一律使用下拉菜单选择（已移除手动输入路径）。
 * - 没有任何文件 → 显示「无文件」提示，引导用户点「导入」上传。
 * - 当前值不在列表中（如旧配置指向已删除文件）→ 仍保留为一个可见选项，不丢数据。
 */
export default function AssetPicker({
  value, onChange, options,
  emptyHint = '（该目录暂无文件，请点「导入」上传）',
  accept, uploading, onUpload,
}: AssetPickerProps) {
  const fileRef = useRef<HTMLInputElement>(null)

  const orphan  = value && !options.includes(value) ? [value] : []
  const allOpts = [...options, ...orphan]

  const uploadBtn = onUpload && (
    <>
      <input
        ref={fileRef}
        type="file"
        accept={accept}
        style={{ display: 'none' }}
        onChange={e => {
          const f = e.target.files?.[0]
          e.target.value = ''                 // 允许再次选择同名文件
          if (f) onUpload(f)
        }}
      />
      <button
        type="button"
        className="picker-toggle"
        disabled={uploading}
        onClick={() => fileRef.current?.click()}
        title="从本机导入文件到 assets/"
      >
        {uploading ? '…' : '导入'}
      </button>
    </>
  )

  return (
    <div className="asset-picker">
      {allOpts.length === 0 ? (
        <span className="picker-empty">{emptyHint}</span>
      ) : (
        <select value={value} onChange={e => onChange(e.target.value)}>
          <option value="">— 选择文件 —</option>
          {allOpts.map(o => <option key={o} value={o}>{o}</option>)}
        </select>
      )}
      {uploadBtn}
    </div>
  )
}
