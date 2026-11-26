import { useEffect, useRef, useState } from 'react'
import { apiErrorMessage } from '../../api/http'

export interface ModelImportResult { path: string; renamed: boolean }

export default function ModelAssetInput({ title, value, options, extension, required, onChange, onImport }: {
  title: string; value: string; options: string[]; extension: '.rknn' | '.txt'; required?: boolean
  onChange: (value: string) => void
  onImport?: (file: File, onProgress: (value: number) => void) => Promise<ModelImportResult>
}) {
  const inputRef = useRef<HTMLInputElement>(null)
  const mounted = useRef(true)
  const [progress, setProgress] = useState<number | null>(null)
  const [message, setMessage] = useState('')
  const [error, setError] = useState('')
  useEffect(() => {
    mounted.current = true
    return () => { mounted.current = false }
  }, [])
  // Keep a canvas-provided path visible even if its package was removed meanwhile.
  const paths = [...new Set([...options, ...(value ? [value] : [])])]
  const kind = extension === '.rknn' ? '模型' : '标签'
  const importAsset = async (file: File) => {
    if (!onImport || progress !== null) return
    setError(''); setMessage('')
    if (!file.name.toLowerCase().endsWith(extension)) { setError(`请选择 ${extension} ${kind}文件`); return }
    if (!file.size) { setError(`${kind}文件为空，请重新选择`); return }
    if (file.size > 1024 ** 3) { setError(`单个${kind}文件不能超过 1 GiB`); return }
    setProgress(0)
    try {
      const result = await onImport(file, value => { if (mounted.current) setProgress(value) })
      if (!mounted.current) return
      onChange(result.path)
      setMessage(result.renamed ? '已导入，同名文件已另存' : '已导入')
    } catch (error) {
      if (mounted.current) setError(apiErrorMessage(error))
    } finally {
      if (mounted.current) setProgress(null)
    }
  }
  return <div className="image-model-asset">
    <label>
      {extension === '.rknn' ? '模型文件' : required ? '标签' : '标签（可选）'}
      <select aria-label={title} value={value} required={required} title={value || title}
        onChange={event => {
          if (event.target.value === '__import__') {
            // Cancelling the file chooser leaves the previous selection intact.
            event.currentTarget.value = value
            inputRef.current?.click()
          } else {
            setError(''); setMessage('')
            onChange(event.target.value)
          }
        }}>
        <option value="">{options.length ? `选择${kind}` : `暂无 ${extension} 文件`}</option>
        {paths.map(path => <option key={path} value={path}>{path.split('/').pop()}</option>)}
        {onImport && <option value="__import__">导入{kind}…</option>}
      </select>
    </label>
    {onImport && <input ref={inputRef} type="file" accept={extension} hidden aria-label={`导入${title}`}
      onChange={event => {
        const file = event.target.files?.[0]
        event.target.value = ''
        if (file) void importAsset(file)
      }} />}
    {progress !== null && <div className="image-import-feedback" role="status">
      {progress >= 100 ? '正在保存…' : `正在上传 ${progress}%`}
      <progress aria-label={`${title}导入进度`} max={100} value={progress} />
    </div>}
    {error && <div className="image-import-feedback image-error-text" role="alert">导入失败：{error}</div>}
    {message && <div className="image-import-feedback" role="status">{message}</div>}
  </div>
}
