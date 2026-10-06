export interface LocalWritable {
  write(data: Uint8Array | Blob): Promise<void>
  close(): Promise<void>
  abort(): Promise<void>
}

export interface LocalFile {
  name: string
  createWritable(): Promise<LocalWritable>
}

export interface LocalDirectory {
  name: string
  getFileHandle(name: string, options: { create: boolean }): Promise<LocalFile>
}

const localWindow = window as typeof window & {
  showSaveFilePicker?: (options: { suggestedName: string; types: Array<{
    description: string; accept: Record<string, string[]>
  }> }) => Promise<LocalFile>
  showDirectoryPicker?: (options: { mode: 'readwrite' }) => Promise<LocalDirectory>
}

export const canChooseLocalFile = !!window.isSecureContext && typeof localWindow.showSaveFilePicker === 'function'
export const canChooseLocalDirectory = !!window.isSecureContext && typeof localWindow.showDirectoryPicker === 'function'
export const localDownloadHint = '文件保存在访问网页的电脑上。若浏览器不弹出保存窗口，请在浏览器设置中开启“下载前询问保存位置”。'

export const chooseLocalFile = (filename: string, mime: string, extension: string) =>
  canChooseLocalFile ? localWindow.showSaveFilePicker!({ suggestedName: filename,
    types: [{ description: extension === '.mp4' ? 'MP4 视频' : '原始图片压缩包', accept: { [mime]: [extension] } }],
  }) : Promise.resolve(null)

export const chooseLocalDirectory = () => canChooseLocalDirectory
  ? localWindow.showDirectoryPicker!({ mode: 'readwrite' }) : Promise.resolve(null)

export function downloadToComputer(url: string, filename: string): void {
  const link = document.createElement('a')
  link.href = url
  link.download = filename
  document.body.appendChild(link)
  link.click()
  link.remove()
}

export async function writeResponseToFile(response: Response, file: LocalFile): Promise<void> {
  if (!response.ok || !response.body) {
    let message = `下载失败（HTTP ${response.status}）`
    try { message = (await response.json()).detail || message } catch { /* 无 JSON 错误内容 */ }
    throw new Error(message)
  }
  const reader = response.body.getReader()
  let writer: LocalWritable | null = null
  try {
    writer = await file.createWritable()
    while (true) {
      const { value, done } = await reader.read()
      if (done) break
      if (value) await writer.write(value)
    }
    await writer.close()
  } catch (error) {
    await reader.cancel().catch(() => {})
    await writer?.abort().catch(() => {})
    throw error
  } finally {
    reader.releaseLock()
  }
}

export const isSaveCancelled = (error: unknown) => error instanceof DOMException && error.name === 'AbortError'
