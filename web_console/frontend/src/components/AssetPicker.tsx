import { useRef, useState, useEffect, useCallback } from 'react'

interface AssetPickerProps {
  value: string
  onChange: (v: string) => void
  options: string[]
  emptyHint?: string
  accept?: string
  uploading?: boolean
  progress?: number
  onUpload?: (file: File) => void
  onDelete?: (path: string) => Promise<void>
}

export default function AssetPicker({
  value, onChange, options,
  emptyHint = '（该目录暂无文件，请点「导入」上传）',
  accept, uploading, progress = 0, onUpload,
  onDelete,
}: AssetPickerProps) {
  const fileRef = useRef<HTMLInputElement>(null)
  const dropdownRef = useRef<HTMLDivElement>(null)
  const [open, setOpen] = useState(false)
  const [deleting, setDeleting] = useState<string | null>(null)

  const orphan  = value && !options.includes(value) ? [value] : []
  const allOpts = [...options, ...orphan]
  const displayName = (p: string) => p.split('/').pop() ?? p

  // close dropdown on outside click
  const handleClickOutside = useCallback((e: MouseEvent) => {
    if (dropdownRef.current && !dropdownRef.current.contains(e.target as Node)) {
      setOpen(false)
    }
  }, [])

  useEffect(() => {
    if (open) {
      document.addEventListener('mousedown', handleClickOutside)
      return () => document.removeEventListener('mousedown', handleClickOutside)
    }
  }, [open, handleClickOutside])

  const handleDelete = async (e: React.MouseEvent, p: string) => {
    e.stopPropagation()
    if (!onDelete) return
    const name = displayName(p)
    if (!window.confirm(`确定要删除远程 RK3588 上的文件「${name}」吗？\n\n此操作不可撤销。`)) return
    setDeleting(p)
    try {
      await onDelete(p)
    } catch (err: unknown) {
      const msg = (err as { response?: { data?: { detail?: string } } })?.response?.data?.detail
        ?? (err instanceof Error ? err.message : String(err))
      window.alert(`删除失败：${msg}`)
    } finally {
      setDeleting(null)
    }
  }

  const selectAndClose = (p: string) => {
    onChange(p)
    setOpen(false)
  }

  const uploadBtn = onUpload && (
    <>
      <input
        ref={fileRef}
        type="file"
        accept={accept}
        style={{ display: 'none' }}
        onChange={e => {
          const f = e.target.files?.[0]
          e.target.value = ''
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
        {uploading ? `${progress}%` : '导入'}
      </button>
    </>
  )

  return (
    <div className="asset-picker-col">
      <div className="asset-picker" ref={dropdownRef}>
        {allOpts.length === 0 ? (
          <span className="picker-empty">{emptyHint}</span>
        ) : (
          <div className="picker-dropdown">
            <button
              type="button"
              className={`picker-trigger ${open ? 'open' : ''}`}
              onClick={() => setOpen(o => !o)}
            >
              <span className={value ? '' : 'placeholder'}>
                {value ? displayName(value) : '— 选择文件 —'}
              </span>
              <span className="picker-arrow">{open ? '▴' : '▾'}</span>
            </button>
            {open && (
              <div className="picker-menu">
                {allOpts.map(p => (
                  <div
                    key={p}
                    className={`picker-item ${p === value ? 'selected' : ''} ${deleting === p ? 'deleting' : ''}`}
                  >
                    <span
                      className="picker-item-name"
                      onClick={() => selectAndClose(p)}
                      title={p}
                    >
                      {displayName(p)}
                    </span>
                    {onDelete && (
                      <button
                        type="button"
                        className="picker-delete-btn"
                        disabled={deleting === p}
                        onClick={e => handleDelete(e, p)}
                        title={`删除 ${displayName(p)}`}
                      >
                        🗑
                      </button>
                    )}
                  </div>
                ))}
              </div>
            )}
          </div>
        )}
        {uploadBtn}
      </div>
      {uploading && (
        <div className="picker-progress" aria-hidden="true">
          <div className="picker-progress-bar" style={{ width: `${progress}%` }} />
        </div>
      )}
    </div>
  )
}
