import { useEffect, useMemo, useRef, useState } from 'react'
import {
  capturePreviewStreamUrl,
  fetchCaptureDevices,
  fetchCaptureStatus,
  fetchCaptureStorage,
  startCapturePreview,
  startCaptureRecording,
  stopCapturePreview,
  stopCaptureRecording,
  videoCaptureErrorMessage,
  type CaptureSourceInput,
  type CaptureSourceType,
  type CaptureStatus,
  type CaptureStorageInfo,
  type UsbCaptureDevice,
  type UsbCaptureResolution,
} from '../api/videoCapture'
import './VideoCapturePage.css'


const MIB = 1024 * 1024
const DEFAULT_RTSP_URL = 'rtsp://admin:jndxc301@192.168.2.150/Streaming/Channels/101'

const formatBytes = (value: number): string => {
  if (!Number.isFinite(value) || value <= 0) return '0 B'
  const units = ['B', 'KB', 'MB', 'GB', 'TB']
  let amount = value
  let unit = 0
  while (amount >= 1024 && unit < units.length - 1) {
    amount /= 1024
    unit += 1
  }
  return `${amount >= 100 || unit === 0 ? amount.toFixed(0) : amount.toFixed(1)} ${units[unit]}`
}

const formatDuration = (seconds: number): string => {
  const total = Math.max(0, Math.floor(seconds))
  const hours = Math.floor(total / 3600)
  const minutes = Math.floor((total % 3600) / 60)
  const secs = total % 60
  return [hours, minutes, secs].map(value => String(value).padStart(2, '0')).join(':')
}

const stopReasonText: Record<string, string> = {
  manual: '已手动停止并保存',
  size_limit: '达到单文件大小上限，已自动停止并保存',
  storage_guard: '达到磁盘保护线，已自动停止并保存',
  service_shutdown: '服务退出前已安全停止并保存',
  source_ended: '视频源已结束',
}

const emptyStatus: CaptureStatus = {
  state: 'idle', recording: false, previewing: false,
  source: null, probe: null, started_unix_ms: null, elapsed_seconds: 0,
  output_path: null, file_size_bytes: 0, max_file_size_bytes: 0,
  stop_reason: null, error: null, process_alive: false, storage: null,
}

export default function VideoCapturePage() {
  const [sourceType, setSourceType] = useState<CaptureSourceType>('rtsp')
  const [rtspUrl, setRtspUrl] = useState(DEFAULT_RTSP_URL)
  const [usbDevice, setUsbDevice] = useState('/dev/video0')
  const [usbWidth, setUsbWidth] = useState(0)
  const [usbHeight, setUsbHeight] = useState(0)
  const [usbDevices, setUsbDevices] = useState<UsbCaptureDevice[]>([])
  const [savePath, setSavePath] = useState('/userdata')
  const [maxFileSizeMb, setMaxFileSizeMb] = useState(1024)
  const [storage, setStorage] = useState<CaptureStorageInfo | null>(null)
  const [storageError, setStorageError] = useState('')
  const [storageLoading, setStorageLoading] = useState(false)
  const [status, setStatus] = useState<CaptureStatus>(emptyStatus)
  const [busy, setBusy] = useState<'preview' | 'record' | 'stop' | ''>('')
  const [actionError, setActionError] = useState('')
  const [previewNonce, setPreviewNonce] = useState(Date.now())
  const [previewLoading, setPreviewLoading] = useState(false)
  const [previewError, setPreviewError] = useState(false)
  const statusRef = useRef(status)

  statusRef.current = status

  const source = useMemo<CaptureSourceInput>(() => ({
    source_type: sourceType,
    rtsp_url: sourceType === 'rtsp' ? rtspUrl.trim() : '',
    usb_device: sourceType === 'usb' ? usbDevice.trim() : '',
    usb_width: sourceType === 'usb' ? usbWidth : 0,
    usb_height: sourceType === 'usb' ? usbHeight : 0,
  }), [sourceType, rtspUrl, usbDevice, usbWidth, usbHeight])

  const activeUsbDevice = useMemo(
    () => usbDevices.find(item => item.device === usbDevice) ?? null,
    [usbDevices, usbDevice],
  )
  const usbResolutionValue = usbWidth > 0 && usbHeight > 0 ? `${usbWidth}x${usbHeight}` : ''

  const sourceReady = sourceType === 'rtsp'
    ? /^rtsps?:\/\//i.test(rtspUrl.trim())
    : /^\/dev\/video\d+$/.test(usbDevice.trim())
  const requestedBytes = Math.max(0, Number(maxFileSizeMb) || 0) * MIB
  const capacityReady = storage?.writable === true
    && requestedBytes >= 64 * MIB
    && requestedBytes <= storage.max_recording_file_bytes
  const controlsLocked = status.recording || status.state === 'stopping'

  const loadStorage = async (path = savePath) => {
    const normalized = path.trim()
    if (!normalized) {
      setStorage(null)
      setStorageError('请输入板端保存路径')
      return
    }
    setStorageLoading(true)
    try {
      const result = await fetchCaptureStorage(normalized)
      setStorage(result)
      setStorageError('')
    } catch (error) {
      setStorage(null)
      setStorageError(videoCaptureErrorMessage(error))
    } finally {
      setStorageLoading(false)
    }
  }

  useEffect(() => {
    let disposed = false
    fetchCaptureDevices()
      .then(devices => {
        if (disposed) return
        setUsbDevices(devices)
        if (devices.length > 0) {
          const selected = devices.find(item => item.device === usbDevice) ?? devices[0]
          setUsbDevice(selected.device)
          const resolution = selected.resolutions[0]
          setUsbWidth(resolution?.width ?? 0)
          setUsbHeight(resolution?.height ?? 0)
        }
      })
      .catch(() => {})
    return () => { disposed = true }
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    let disposed = false
    let pending = false
    const load = async () => {
      if (pending) return
      pending = true
      try {
        const result = await fetchCaptureStatus()
        if (!disposed) setStatus(result)
      } catch (error) {
        if (!disposed) setActionError(videoCaptureErrorMessage(error))
      } finally {
        pending = false
      }
    }
    load()
    const timer = window.setInterval(load, 1000)
    return () => {
      disposed = true
      window.clearInterval(timer)
    }
  }, [])

  useEffect(() => {
    let disposed = false
    const timer = window.setTimeout(() => {
      if (!disposed) void loadStorage(savePath)
    }, 350)
    const refresh = window.setInterval(() => {
      if (!disposed) void loadStorage(savePath)
    }, 5000)
    return () => {
      disposed = true
      window.clearTimeout(timer)
      window.clearInterval(refresh)
    }
  }, [savePath]) // eslint-disable-line react-hooks/exhaustive-deps

  // 录像由板端独立维持；刷新页面重新进入时，以后端正在使用的真实路径为准。
  useEffect(() => {
    if (status.recording && status.storage?.path && status.storage.path !== savePath) {
      setSavePath(status.storage.path)
    }
  }, [status.recording, status.storage?.path, savePath])

  useEffect(() => () => {
    if (!statusRef.current.recording && statusRef.current.state !== 'stopping') {
      void stopCapturePreview().catch(() => {})
    }
  }, [])

  const connectPreview = async () => {
    if (!sourceReady || controlsLocked) return
    setBusy('preview')
    setActionError('')
    setPreviewError(false)
    setPreviewLoading(true)
    try {
      const result = await startCapturePreview(source)
      setStatus(result)
      setPreviewNonce(Date.now())
    } catch (error) {
      setPreviewLoading(false)
      setActionError(videoCaptureErrorMessage(error))
    } finally {
      setBusy('')
    }
  }

  const startRecording = async () => {
    if (!sourceReady || !capacityReady || controlsLocked) return
    setBusy('record')
    setActionError('')
    setPreviewError(false)
    setPreviewLoading(true)
    try {
      // 后端会在启动瞬间再次检查同一路径容量，页面显示不是唯一保护线。
      const result = await startCaptureRecording(source, storage!.path, Math.floor(maxFileSizeMb))
      setStatus(result)
      setPreviewNonce(Date.now())
      await loadStorage(storage!.path)
    } catch (error) {
      setPreviewLoading(false)
      setActionError(videoCaptureErrorMessage(error))
      await loadStorage()
    } finally {
      setBusy('')
    }
  }

  const stopRecording = async () => {
    setBusy('stop')
    setActionError('')
    try {
      const result = await stopCaptureRecording()
      setStatus(result)
      setPreviewLoading(false)
      await loadStorage(result.storage?.path ?? savePath)
    } catch (error) {
      setActionError(videoCaptureErrorMessage(error))
    } finally {
      setBusy('')
    }
  }

  const changeSourceType = (next: CaptureSourceType) => {
    if (controlsLocked || next === sourceType) return
    setSourceType(next)
    setPreviewError(false)
    setPreviewLoading(false)
    void stopCapturePreview().then(setStatus).catch(() => {})
  }

  const changeUsbDevice = (devicePath: string) => {
    setUsbDevice(devicePath)
    const device = usbDevices.find(item => item.device === devicePath)
    const resolution = device?.resolutions[0]
    setUsbWidth(resolution?.width ?? 0)
    setUsbHeight(resolution?.height ?? 0)
    setPreviewError(false)
  }

  const changeUsbResolution = (value: string) => {
    const [width, height] = value.split('x').map(Number)
    setUsbWidth(Number.isFinite(width) ? width : 0)
    setUsbHeight(Number.isFinite(height) ? height : 0)
    setPreviewError(false)
  }

  const resolutionLabel = (resolution: UsbCaptureResolution) => (
    `${resolution.width} × ${resolution.height}`
    + (resolution.max_fps > 0 ? `（最高 ${resolution.max_fps} FPS）` : '')
  )

  const stateText = status.state === 'recording' ? '正在录制'
    : status.state === 'stopping' ? '正在完成 MP4'
      : status.state === 'previewing' ? '正在预览'
        : status.state === 'completed' ? '录制完成'
          : status.state === 'error' ? '采集异常' : '空闲'

  return (
    <div className="video-capture-page">
      <header className="video-capture-header">
        <div>
          <h2>视频采集</h2>
          <p>从 RTSP 或 USB 视频源采集单个 MP4，独立于视觉算法和告警录像。</p>
        </div>
        <span className={`capture-state-badge ${status.state}`}>
          {status.recording && <i />}{stateText}
        </span>
      </header>

      {actionError && <div className="capture-message error">
        <span>{actionError}</span>
        <button onClick={() => setActionError('')}>×</button>
      </div>}
      {status.error && status.error !== actionError && <div className="capture-message error">
        <span>{status.error}</span>
      </div>}
      {status.output_path && !status.recording && status.state !== 'stopping' && (
        <div className={`capture-message ${status.state === 'error' ? 'warning' : 'success'}`}>
          <span>
            {status.stop_reason && stopReasonText[status.stop_reason]
              ? `${stopReasonText[status.stop_reason]}：` : '录像文件：'}
            <code>{status.output_path}</code>
          </span>
        </div>
      )}

      <div className="video-capture-workspace">
        <section className="capture-preview-panel">
          <div className="capture-panel-title">
            <span>原始画面预览</span>
            {(status.source || status.probe) && <small title={status.source?.label ?? ''}>
              {status.source?.label ?? ''}
              {status.probe ? ` · ${status.probe.codec.toUpperCase()}` : ''}
              {status.probe && status.probe.width > 0 && status.probe.height > 0
                ? ` · ${status.probe.width}×${status.probe.height}` : ''}
              {status.probe && status.probe.fps > 0 ? ` · ${status.probe.fps} FPS` : ''}
            </small>}
          </div>
          <div className="capture-preview-stage">
            {status.previewing && !previewError ? <>
              <img
                key={previewNonce}
                src={capturePreviewStreamUrl(previewNonce)}
                className={previewLoading ? 'loading' : ''}
                alt="原始视频预览"
                onLoad={() => { setPreviewLoading(false); setPreviewError(false) }}
                onError={() => { setPreviewLoading(false); setPreviewError(true) }}
              />
              {previewLoading && <div className="capture-preview-placeholder">
                <span className="capture-spinner" />正在连接原始视频……
              </div>}
              {status.recording && <div className="capture-record-overlay">
                <span className="capture-record-dot" />
                REC {formatDuration(status.elapsed_seconds)} · {formatBytes(status.file_size_bytes)}
              </div>}
            </> : <div className="capture-preview-placeholder">
              <span className="capture-preview-icon">▰</span>
              <strong>{previewError ? '预览连接已结束' : '尚未连接视频源'}</strong>
              <span>填写来源后点击“连接预览”。录像由板端执行，页面关闭不会停止录像。</span>
            </div>}
          </div>
        </section>

        <section className="capture-settings-panel">
          <div className="capture-panel-title"><span>采集设置</span></div>
          <div className="capture-settings-scroll">
            <div className="capture-section">
              <div className="capture-section-label">视频来源</div>
              <div className="capture-source-tabs">
                <button className={sourceType === 'rtsp' ? 'active' : ''}
                  disabled={controlsLocked} onClick={() => changeSourceType('rtsp')}>RTSP</button>
                <button className={sourceType === 'usb' ? 'active' : ''}
                  disabled={controlsLocked} onClick={() => changeSourceType('usb')}>USB</button>
              </div>
              {sourceType === 'rtsp' ? <div className="capture-field">
                <label>RTSP 地址</label>
                <input value={rtspUrl} disabled={controlsLocked}
                  placeholder="rtsp://user:password@192.168.1.64/Streaming/Channels/101"
                  onChange={event => setRtspUrl(event.target.value)} />
                <small>H264 原码流直接写入 MP4；H265 由 RK3588 硬件转换为标准 H264，兼容 H265/H265+ 摄像头。</small>
              </div> : <div className="capture-field">
                <label>USB 视频设备</label>
                {usbDevices.length > 0 ? <select value={usbDevice} disabled={controlsLocked}
                  onChange={event => changeUsbDevice(event.target.value)}>
                  {usbDevices.map(device => <option key={device.device} value={device.device}>
                    {device.device} · {device.label}{device.readable ? '' : '（不可读）'}
                  </option>)}
                </select> : <input value={usbDevice} disabled={controlsLocked}
                  placeholder="/dev/video0" onChange={event => setUsbDevice(event.target.value)} />}
                <label className="capture-subfield-label">采集分辨率</label>
                <select value={usbResolutionValue} disabled={controlsLocked || !activeUsbDevice?.resolutions.length}
                  onChange={event => changeUsbResolution(event.target.value)}>
                  {!activeUsbDevice?.resolutions.length && <option value="">使用设备当前分辨率</option>}
                  {activeUsbDevice?.resolutions.map(resolution => <option
                    key={`${resolution.width}x${resolution.height}`}
                    value={`${resolution.width}x${resolution.height}`}>
                    {resolutionLabel(resolution)}
                  </option>)}
                </select>
                <small>USB 的 MJPEG/NV12/YUYV 使用色彩安全的软件 H264 编码，兼顾文件体积、非 16 对齐分辨率和色彩准确性。</small>
              </div>}
              <button className="capture-secondary-button" disabled={!sourceReady || controlsLocked || busy !== ''}
                onClick={connectPreview}>
                {busy === 'preview' ? '正在连接……' : status.previewing ? '重新连接预览' : '连接预览'}
              </button>
            </div>

            <div className="capture-section">
              <div className="capture-section-label">文件保存</div>
              <div className="capture-field">
                <label>板端保存路径</label>
                <div className="capture-path-row">
                  <input value={savePath} disabled={controlsLocked}
                    placeholder="/mnt/recordings" onChange={event => setSavePath(event.target.value)} />
                  <button disabled={storageLoading} onClick={() => void loadStorage()} title="刷新容量">↻</button>
                </div>
                <small>目录必须已经存在；录像文件直接写入该目录，不创建子文件夹。</small>
              </div>

              {storageLoading && !storage && <div className="capture-storage-loading">正在读取存储空间……</div>}
              {storageError && <div className="capture-storage-error">{storageError}</div>}
              {storage && <div className={`capture-storage-card ${storage.writable ? '' : 'invalid'}`}>
                <div className="capture-storage-head">
                  <strong>{storage.mount_point}</strong>
                  <span>{storage.filesystem || 'unknown'}</span>
                </div>
                <div className="capture-storage-meter">
                  <i style={{ width: `${Math.min(100, storage.total_bytes > 0
                    ? storage.used_bytes * 100 / storage.total_bytes : 0)}%` }} />
                </div>
                <div className="capture-storage-grid">
                  <span>磁盘总容量<strong>{formatBytes(storage.total_bytes)}</strong></span>
                  <span>当前剩余<strong>{formatBytes(storage.available_bytes)}</strong></span>
                  <span>安全保留<strong>{formatBytes(storage.reserve_bytes)}</strong></span>
                  <span>最大可录文件<strong>{formatBytes(storage.max_recording_file_bytes)}</strong></span>
                </div>
                {!storage.writable && <div className="capture-storage-warning">该目录不可写，不能开始录像。</div>}
              </div>}

              <div className="capture-field capture-size-field">
                <label>单个 MP4 最大大小（MB）</label>
                <input type="number" min={64} max={1024 * 1024} step={64}
                  value={maxFileSizeMb} disabled={controlsLocked}
                  onChange={event => setMaxFileSizeMb(Number(event.target.value))} />
                <small>达到上限后自动停止并完成当前 MP4，不生成第二个文件。</small>
              </div>
              {storage && requestedBytes > storage.max_recording_file_bytes && (
                <div className="capture-storage-error">
                  设置的 {formatBytes(requestedBytes)} 超过该路径最大安全可录文件
                  {formatBytes(storage.max_recording_file_bytes)}，请降低上限或更换路径。
                </div>
              )}
            </div>
          </div>

          <div className="capture-actions">
            {status.recording || status.state === 'stopping' ? <button
              className="capture-stop-button" disabled={busy !== '' || status.state === 'stopping'}
              onClick={stopRecording}>
              {status.state === 'stopping' || busy === 'stop' ? '正在完成 MP4……' : '停止并保存'}
            </button> : <button className="capture-record-button"
              disabled={!sourceReady || !capacityReady || busy !== ''}
              title={!sourceReady ? '请先填写视频来源'
                : !storage ? '请先确认保存路径和剩余空间'
                  : !capacityReady ? '单文件大小超过安全可用空间' : ''}
              onClick={startRecording}>
              <span />{busy === 'record' ? '正在启动……' : '开始录制'}
            </button>}
            <div className="capture-action-hint">
              {status.recording
                ? `${formatDuration(status.elapsed_seconds)} · ${formatBytes(status.file_size_bytes)} / ${formatBytes(status.max_file_size_bytes)}`
                : capacityReady ? `本次最多写入 ${formatBytes(requestedBytes)}` : '确认来源和存储空间后才能录制'}
            </div>
          </div>
        </section>
      </div>
    </div>
  )
}
