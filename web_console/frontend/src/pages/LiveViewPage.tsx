import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'
import axios from 'axios'
import {
  fetchApps,
  fetchFilePlayback,
  fetchLogicControls,
  fetchConfig,
  loadConfigFile,
  sendChannelAction,
  seekFilePlayback,
  sendGlobalLogicAction,
  streamUrl,
  type AppInfo,
  type FilePlaybackSource,
  type LogicControlsResponse,
  type LogicActionDef,
} from '../api/client'
import { useAuthStore } from '../store/authStore'
import { videoTimelineLayout, type VideoLayout } from './videoTimelineLayout'
import './LiveViewPage.css'
import { LivePlaybackBuffer, livePlaybackRate, livePlaybackTarget } from './livePlayback'
import LiveLogConsole from '../components/LiveLogConsole'
import { PlaybackProgress, StreamOperationTimeout, readStreamChunk, sourceBufferOperation, streamRetryDelay, waitForSourceOpen } from './liveStreamRecovery'

type RtspState = 'idle' | 'checking' | 'enabled' | 'disabled' | 'error'
type StreamRetryCause = 'transport-error' | 'read-timeout' | 'decoder-error' | 'startup-timeout' | 'playback-stall' | 'buffer-timeout' | 'initialization-timeout'

const STREAM_MAX_RETRY = 25
const STREAM_STALL_MS = 15000
const STREAM_INIT_LIMIT = 4 * 1024 * 1024
const LIVE_SIDE_WIDTH_STORAGE_KEY = 'rk3588.liveView.sidePanelWidth'
const LIVE_SIDE_WIDTH_DEFAULT = 380
const LIVE_SIDE_WIDTH_MIN = 310
const LIVE_SIDE_WIDTH_MAX = 900
const FILE_TIMELINE_HIDE_MS = 3000

function storedSidePanelWidth(): number {
  try {
    const value = Number(window.localStorage.getItem(LIVE_SIDE_WIDTH_STORAGE_KEY))
    return Number.isFinite(value) && value > 0
      ? Math.min(LIVE_SIDE_WIDTH_MAX, Math.max(LIVE_SIDE_WIDTH_MIN, value))
      : LIVE_SIDE_WIDTH_DEFAULT
  } catch {
    return LIVE_SIDE_WIDTH_DEFAULT
  }
}

function saveSidePanelWidth(value: number) {
  try {
    window.localStorage.setItem(LIVE_SIDE_WIDTH_STORAGE_KEY, String(Math.round(value)))
  } catch {
    // 隐私模式禁用 localStorage 时仍允许本次页面正常拖拽。
  }
}

class FatalStreamError extends Error {}

function concatBytes(parts: Uint8Array[], total: number): Uint8Array {
  const merged = new Uint8Array(total)
  let offset = 0
  for (const part of parts) {
    merged.set(part, offset)
    offset += part.byteLength
  }
  return merged
}

/** SourceBuffer 不接受 SharedArrayBuffer；普通 fetch 数据保持零复制，仅截取有效视图范围。 */
function sourceBufferBytes(bytes: Uint8Array<ArrayBufferLike>): ArrayBuffer {
  if (bytes.buffer instanceof ArrayBuffer) {
    if (bytes.byteOffset === 0 && bytes.byteLength === bytes.buffer.byteLength) return bytes.buffer
    return bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength)
  }
  const owned = new Uint8Array(bytes.byteLength)
  owned.set(bytes)
  return owned.buffer
}

/** 从 MP4 初始化段的 avcC box 读取真实 H264 profile/compatibility/level。 */
function findAvcCodec(data: Uint8Array): string | null {
  for (let i = 4; i + 8 <= data.byteLength; i += 1) {
    if (data[i] !== 0x61 || data[i + 1] !== 0x76 || data[i + 2] !== 0x63 || data[i + 3] !== 0x43) continue
    const hex = (value: number) => value.toString(16).padStart(2, '0').toUpperCase()
    return `avc1.${hex(data[i + 5])}${hex(data[i + 6])}${hex(data[i + 7])}`
  }
  return null
}

function errMsg(error: unknown): string {
  if (axios.isAxiosError(error)) {
    return error.response?.data?.detail ?? error.response?.data?.message ?? error.message
  }
  return error instanceof Error ? error.message : String(error)
}

function mediaErrorMessage(error: MediaError | null): string {
  if (!error) return '浏览器视频解码器报错'
  if (error.message) return `浏览器视频解码失败：${error.message}`
  const descriptions: Record<number, string> = {
    1: '视频加载被中止',
    2: '视频数据传输失败',
    3: '浏览器无法解码该 H264 视频',
    4: '浏览器不支持该视频格式',
  }
  return descriptions[error.code] ?? `浏览器视频错误（代码 ${error.code}）`
}

function runtimeConfigPath(configName: string): string | null {
  const name = configName.trim()
  if (!name || name === 'config.json') return null
  return name.startsWith('assets/') ? name : `assets/${name}`
}

function formatPlaybackTime(milliseconds: number): string {
  const totalSeconds = Math.max(0, Math.floor(milliseconds / 1000))
  const hours = Math.floor(totalSeconds / 3600)
  const minutes = Math.floor((totalSeconds % 3600) / 60)
  const seconds = totalSeconds % 60
  const short = `${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`
  return hours > 0 ? `${String(hours).padStart(2, '0')}:${short}` : short
}

function sourceFileName(location: string): string {
  const normalized = location.replace(/\\/g, '/')
  return normalized.split('/').filter(Boolean).pop() ?? location
}

export default function LiveViewPage() {
  const [apps, setApps] = useState<AppInfo[]>([])
  const [appsLoading, setAppsLoading] = useState(true)
  const [appsError, setAppsError] = useState('')
  const [rtspState, setRtspState] = useState<RtspState>('idle')
  const [streamErr, setStreamErr] = useState(false)
  const [streamErrorDetail, setStreamErrorDetail] = useState('')
  const [streamLoading, setStreamLoading] = useState(true)
  const [streamNonce, setStreamNonce] = useState(0)
  const [streamRetryWaitMs, setStreamRetryWaitMs] = useState(0)
  const [streamClientId] = useState(() => Array.from(crypto.getRandomValues(new Uint8Array(16)),
    byte => byte.toString(16).padStart(2, '0')).join(''))
  const [snapshotReady, setSnapshotReady] = useState(false)
  const [videoFullscreen, setVideoFullscreen] = useState(false)
  const [controls, setControls] = useState<LogicControlsResponse | null>(null)
  const [fileSources, setFileSources] = useState<FilePlaybackSource[]>([])
  const [seekDrafts, setSeekDrafts] = useState<Record<number, number>>({})
  const [seekBusy, setSeekBusy] = useState<Record<number, boolean>>({})
  const [activeTimelineChannel, setActiveTimelineChannel] = useState<number | null>(null)
  const [videoLayout, setVideoLayout] = useState<VideoLayout | null>(null)
  const [pictureBounds, setPictureBounds] = useState({ left: 0, top: 0, width: 0, height: 0 })
  const [actionBusy, setActionBusy] = useState<Record<string, boolean>>({})
  const [toast, setToast] = useState<{ msg: string; type: 'ok' | 'err' } | null>(null)
  const [sidePanelWidth, setSidePanelWidth] = useState(storedSidePanelWidth)

  const workspaceRef = useRef<HTMLDivElement>(null)
  const streamRetryRef = useRef(0)
  const streamRetryTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const streamRetryPendingRef = useRef(false)
  const streamLastErrorRef = useRef('')
  const streamRetryDiagnosticsRef = useRef<Record<string, string>>({})
  const videoRef = useRef<HTMLVideoElement>(null)
  const videoSnapshotRef = useRef<HTMLCanvasElement>(null)
  const videoFrameRef = useRef<HTMLDivElement>(null)
  const toastTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const seekDraftsRef = useRef<Record<number, number>>({})
  const seekBusyRef = useRef(new Set<number>())
  const fileTimelineTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null)
  const fileTimelinePointerRef = useRef<{ pointerId: number; channelId: number } | null>(null)
  const sideResizeRef = useRef<{
    pointerId: number
    startX: number
    startWidth: number
    latestWidth: number
    previousCursor: string
    previousUserSelect: string
  } | null>(null)

  useEffect(() => {
    const fitSidePanel = () => {
      if (window.innerWidth <= 1080) return
      const available = (workspaceRef.current?.clientWidth ?? window.innerWidth) - 420
      const max = Math.min(LIVE_SIDE_WIDTH_MAX, Math.max(LIVE_SIDE_WIDTH_MIN, available))
      setSidePanelWidth(value => Math.min(value, max))
    }
    fitSidePanel()
    window.addEventListener('resize', fitSidePanel)
    return () => {
      window.removeEventListener('resize', fitSidePanel)
      const drag = sideResizeRef.current
      if (drag) {
        document.body.style.cursor = drag.previousCursor
        document.body.style.userSelect = drag.previousUserSelect
        sideResizeRef.current = null
      }
    }
  }, [])

  const beginSideResize = useCallback((event: React.PointerEvent<HTMLDivElement>) => {
    event.preventDefault()
    event.currentTarget.setPointerCapture(event.pointerId)
    sideResizeRef.current = {
      pointerId: event.pointerId,
      startX: event.clientX,
      startWidth: sidePanelWidth,
      latestWidth: sidePanelWidth,
      previousCursor: document.body.style.cursor,
      previousUserSelect: document.body.style.userSelect,
    }
    document.body.style.cursor = 'col-resize'
    document.body.style.userSelect = 'none'
  }, [sidePanelWidth])

  const moveSideResize = useCallback((event: React.PointerEvent<HTMLDivElement>) => {
    const drag = sideResizeRef.current
    if (!drag || drag.pointerId !== event.pointerId) return
    const requested = drag.startWidth + drag.startX - event.clientX
    const available = (workspaceRef.current?.clientWidth ?? window.innerWidth) - 420
    const max = Math.min(LIVE_SIDE_WIDTH_MAX, Math.max(LIVE_SIDE_WIDTH_MIN, available))
    const next = Math.min(max, Math.max(LIVE_SIDE_WIDTH_MIN, requested))
    drag.latestWidth = next
    setSidePanelWidth(next)
  }, [])

  const endSideResize = useCallback((event: React.PointerEvent<HTMLDivElement>) => {
    const drag = sideResizeRef.current
    if (!drag || drag.pointerId !== event.pointerId) return
    saveSidePanelWidth(drag.latestWidth)
    document.body.style.cursor = drag.previousCursor
    document.body.style.userSelect = drag.previousUserSelect
    sideResizeRef.current = null
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId)
    }
  }, [])

  const resizeSideByKeyboard = useCallback((event: React.KeyboardEvent<HTMLDivElement>) => {
    if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return
    event.preventDefault()
    const available = (workspaceRef.current?.clientWidth ?? window.innerWidth) - 420
    const max = Math.min(LIVE_SIDE_WIDTH_MAX, Math.max(LIVE_SIDE_WIDTH_MIN, available))
    const step = event.shiftKey ? 50 : 10
    const next = Math.min(max, Math.max(LIVE_SIDE_WIDTH_MIN,
      sidePanelWidth + (event.key === 'ArrowLeft' ? step : -step)))
    setSidePanelWidth(next)
    saveSidePanelWidth(next)
  }, [sidePanelWidth])

  const runningApp = apps.find(app => app.status === 'running') ?? null
  const appName = runningApp?.name ?? ''
  const runningConfig = runningApp?.config ?? ''
  const rtspEnabled = rtspState === 'enabled'
  const fileTimelineEnabled = !!appName && rtspEnabled && !streamLoading && !streamErr

  const timelineTiles = useMemo(() => fileTimelineEnabled ? videoLayout?.tiles.flatMap(tile => {
    const source = fileSources.find(item => item.channel_ids.includes(tile.channelId))
    return source ? [{ ...tile, source }] : []
  }) ?? [] : [], [fileTimelineEnabled, videoLayout, fileSources])
  const timelineKey = timelineTiles.map(tile => `${tile.channelId}:${tile.source.owner_channel_id}`).join(',')

  const clearFileTimelineTimer = useCallback(() => {
    if (fileTimelineTimerRef.current !== null) clearTimeout(fileTimelineTimerRef.current)
    fileTimelineTimerRef.current = null
  }, [])

  const hideFileTimelines = useCallback(() => {
    if (fileTimelinePointerRef.current !== null) return
    clearFileTimelineTimer()
    setActiveTimelineChannel(null)
  }, [clearFileTimelineTimer])

  const showFileTimeline = useCallback((channelId: number) => {
    const drag = fileTimelinePointerRef.current
    if (drag && drag.channelId !== channelId) return
    setActiveTimelineChannel(channelId)
    clearFileTimelineTimer()
    if (drag) return
    fileTimelineTimerRef.current = setTimeout(() => {
      fileTimelineTimerRef.current = null
      setActiveTimelineChannel(null)
    }, FILE_TIMELINE_HIDE_MS)
  }, [clearFileTimelineTimer])

  useEffect(() => {
    clearFileTimelineTimer()
    fileTimelinePointerRef.current = null
    setActiveTimelineChannel(null)
    const finishInteraction = (event: PointerEvent) => {
      const drag = fileTimelinePointerRef.current
      if (!drag || drag.pointerId !== event.pointerId) return
      fileTimelinePointerRef.current = null
      showFileTimeline(drag.channelId)
    }
    const hideTimelines = () => {
      fileTimelinePointerRef.current = null
      hideFileTimelines()
    }
    window.addEventListener('pointerup', finishInteraction)
    window.addEventListener('pointercancel', finishInteraction)
    window.addEventListener('blur', hideTimelines)
    return () => {
      clearFileTimelineTimer()
      window.removeEventListener('pointerup', finishInteraction)
      window.removeEventListener('pointercancel', finishInteraction)
      window.removeEventListener('blur', hideTimelines)
    }
  }, [appName, runningConfig, timelineKey, clearFileTimelineTimer, hideFileTimelines, showFileTimeline])

  const measurePicture = useCallback(() => {
    const frame = videoFrameRef.current
    const video = videoRef.current
    if (!frame || !video || !videoLayout) return null
    const frameBox = frame.getBoundingClientRect()
    const box = video.getBoundingClientRect()
    const width = video.videoWidth || videoLayout.width
    const height = video.videoHeight || videoLayout.height
    const scale = Math.min(box.width / width, box.height / height)
    return {
      left: box.left - frameBox.left + (box.width - width * scale) / 2,
      top: box.top - frameBox.top + (box.height - height * scale) / 2,
      width: width * scale,
      height: height * scale,
    }
  }, [videoLayout])

  useLayoutEffect(() => {
    const measure = () => {
      const next = measurePicture()
      if (next) setPictureBounds(previous =>
        Object.keys(next).every(key => Math.abs(previous[key as keyof typeof next] - next[key as keyof typeof next]) < 0.1)
          ? previous : next)
    }
    const frame = videoFrameRef.current
    const video = videoRef.current
    const observer = new ResizeObserver(measure)
    if (frame) observer.observe(frame)
    if (video) observer.observe(video)
    video?.addEventListener('loadedmetadata', measure)
    video?.addEventListener('resize', measure)
    window.addEventListener('resize', measure)
    document.addEventListener('fullscreenchange', measure)
    measure()
    return () => {
      observer.disconnect()
      video?.removeEventListener('loadedmetadata', measure)
      video?.removeEventListener('resize', measure)
      window.removeEventListener('resize', measure)
      document.removeEventListener('fullscreenchange', measure)
    }
  }, [appName, streamNonce, rtspEnabled, streamErr, videoFullscreen, measurePicture])

  useEffect(() => {
    if (timelineTiles.length === 0) return
    let lastPosition: { x: number; y: number } | null = null
    const detectMovement = (event: MouseEvent) => {
      const frame = videoFrameRef.current
      if (!frame) return
      if (lastPosition?.x === event.clientX && lastPosition.y === event.clientY) return
      lastPosition = { x: event.clientX, y: event.clientY }
      if (fileTimelinePointerRef.current) return
      const frameBox = frame.getBoundingClientRect()
      if (event.clientX < frameBox.left || event.clientX >= frameBox.right ||
          event.clientY < frameBox.top || event.clientY >= frameBox.bottom) {
        hideFileTimelines()
        return
      }
      const picture = measurePicture()
      if (!picture || picture.width <= 0 || picture.height <= 0) return
      const x = (event.clientX - frameBox.left - picture.left) / picture.width
      const y = (event.clientY - frameBox.top - picture.top) / picture.height
      const tile = timelineTiles.find(item => x >= item.left && x < item.left + item.width &&
        y >= item.top && y < item.top + item.height)
      if (tile) showFileTimeline(tile.channelId)
      else hideFileTimelines()
    }
    const resetPosition = () => {
      lastPosition = null
      hideFileTimelines()
    }
    document.addEventListener('pointermove', detectMovement, true)
    document.addEventListener('mousemove', detectMovement, true)
    document.addEventListener('fullscreenchange', resetPosition)
    return () => {
      document.removeEventListener('pointermove', detectMovement, true)
      document.removeEventListener('mousemove', detectMovement, true)
      document.removeEventListener('fullscreenchange', resetPosition)
    }
  }, [timelineKey, measurePicture, hideFileTimelines, showFileTimeline])

  const focusTimeline = (event: React.FocusEvent<HTMLDivElement>) => {
    // 鼠标点击画面也会聚焦外框；只有键盘聚焦才默认选择第一路。
    if (event.target === event.currentTarget && event.currentTarget.matches(':focus-visible') && timelineTiles[0]) {
      showFileTimeline(timelineTiles[0].channelId)
    }
  }

  const selectTimelineByKeyboard = (event: React.KeyboardEvent<HTMLDivElement>) => {
    if (event.target !== event.currentTarget || !videoLayout || timelineTiles.length === 0) return
    const offsets: Record<string, number> = { ArrowLeft: -1, ArrowRight: 1, ArrowUp: -videoLayout.columns, ArrowDown: videoLayout.columns }
    const offset = offsets[event.key]
    if (offset === undefined) return
    event.preventDefault()
    const index = timelineTiles.findIndex(tile => tile.channelId === activeTimelineChannel)
    const next = Math.min(timelineTiles.length - 1, Math.max(0, index + offset))
    showFileTimeline(timelineTiles[next].channelId)
  }

  const actionTargets = controls ? [
    ...controls.globals.map(instance => ({
      key: `global:${instance.instance_id}`,
      title: '全局逻辑',
      subtitle: instance.logic_label,
      enabled: instance.enabled,
      actions: instance.actions,
      run: (action: LogicActionDef) => sendGlobalLogicAction(
        appName, instance.instance_id, action.id, action.payload ?? {},
      ),
    })),
    ...controls.channels.map(channel => ({
      key: `channel:${channel.channel_id}`,
      title: `通道 ${channel.channel_id}`,
      subtitle: channel.logic_label,
      enabled: channel.enabled,
      actions: channel.actions,
      run: (action: LogicActionDef) => sendChannelAction(
        appName, channel.channel_id, action.id, action.payload ?? {},
      ),
    })),
  ] : []
  const hasActions = actionTargets.some(target => target.actions.length > 0)

  const showToast = (msg: string, type: 'ok' | 'err' = 'ok') => {
    if (toastTimerRef.current) clearTimeout(toastTimerRef.current)
    setToast({ msg, type })
    toastTimerRef.current = null
    if (type === 'ok') {
      toastTimerRef.current = setTimeout(() => {
        setToast(null)
        toastTimerRef.current = null
      }, 3000)
    }
  }

  const clearStreamRetry = () => {
    if (streamRetryTimer.current) clearTimeout(streamRetryTimer.current)
    streamRetryTimer.current = null
    streamRetryPendingRef.current = false
  }

  const scheduleStreamRetry = (reason: string, cause: StreamRetryCause = 'transport-error') => {
    if (reason) streamLastErrorRef.current = reason
    if (streamRetryPendingRef.current) return
    clearStreamRetry()
    if (streamRetryRef.current >= STREAM_MAX_RETRY) {
      const detail = streamLastErrorRef.current || '视频流连续重连失败'
      setStreamLoading(false)
      setStreamErr(true)
      setStreamErrorDetail(detail)
      showToast(detail, 'err')
      return
    }
    streamRetryRef.current += 1
    const delay = streamRetryDelay(streamRetryRef.current)
    streamRetryPendingRef.current = true
    setStreamRetryWaitMs(delay)
    if (reason) {
      setStreamErrorDetail(reason)
      console.warn('[LiveStream] 正在重连:', reason)
    }
    const video = videoRef.current
    const end = video?.buffered.length ? video.buffered.end(video.buffered.length - 1) : 0
    streamRetryDiagnosticsRef.current = {
      retry_cause: cause,
      retry_attempt: String(streamRetryRef.current),
      retry_delay_ms: String(delay),
      retry_position_ms: String(Math.round((video?.currentTime ?? 0) * 1000)),
      retry_ahead_ms: String(Math.round(Math.max(0, end - (video?.currentTime ?? 0)) * 1000)),
      retry_ready_state: String(video?.readyState ?? 0),
      retry_paused: String(video?.paused ?? true),
      retry_seeking: String(video?.seeking ?? false),
    }
    const snapshot = videoSnapshotRef.current
    if (video && snapshot && video.readyState >= 2 && video.videoWidth > 0) {
      const scale = Math.min(1, 1280 / video.videoWidth)
      snapshot.width = Math.round(video.videoWidth * scale)
      snapshot.height = Math.round(video.videoHeight * scale)
      try {
        snapshot.getContext('2d')?.drawImage(video, 0, 0, snapshot.width, snapshot.height)
        setSnapshotReady(true)
      } catch { /* 解码器已释放帧时继续正常重连。 */ }
    }
    setStreamLoading(true)
    streamRetryTimer.current = setTimeout(() => {
      streamRetryPendingRef.current = false
      streamRetryTimer.current = null
      setStreamRetryWaitMs(0)
      setStreamNonce(value => value + 1)
    }, delay)
  }

  const handleStreamLoad = () => {
    // 旧连接残留的 playing 事件不能取消已安排的传输恢复。
    if (streamRetryPendingRef.current) return
    clearStreamRetry()
    setStreamRetryWaitMs(0)
    streamLastErrorRef.current = ''
    streamRetryDiagnosticsRef.current = {}
    setStreamErrorDetail('')
    // video 已经真正进入 playing，立即撤掉遮罩；进度动画绝不延后画面显示。
    setStreamLoading(false)
  }

  const retryStream = () => {
    clearStreamRetry()
    streamRetryRef.current = 0
    setStreamRetryWaitMs(0)
    streamLastErrorRef.current = ''
    streamRetryDiagnosticsRef.current = {}
    setStreamErr(false)
    setStreamErrorDetail('')
    setStreamLoading(true)
    setSnapshotReady(false)
    setStreamNonce(value => value + 1)
  }

  const toggleVideoFullscreen = async () => {
    try {
      if (document.fullscreenElement === videoFrameRef.current) {
        await document.exitFullscreen()
      } else if (videoFrameRef.current) {
        await videoFrameRef.current.requestFullscreen()
      }
    } catch (error) {
      showToast(`切换全屏失败：${errMsg(error)}`, 'err')
    }
  }

  const handleAction = async (
    targetKey: string,
    targetLabel: string,
    action: LogicActionDef,
    run: () => Promise<{ message?: string }>,
  ) => {
    if (!appName) return
    if (action.confirm && !window.confirm(action.confirm)) return
    const key = `${targetKey}:${action.id}`
    setActionBusy(previous => ({ ...previous, [key]: true }))
    try {
      const response = await run()
      showToast(
        response?.message
          ? `${targetLabel}：${response.message}`
          : `${targetLabel}的操作已进入队列`,
      )
    } catch (error) {
      showToast(`${targetLabel}操作失败：${errMsg(error)}`, 'err')
    } finally {
      setActionBusy(previous => ({ ...previous, [key]: false }))
    }
  }

  const setSeekDraft = (ownerChannelId: number, value: number) => {
    seekDraftsRef.current = { ...seekDraftsRef.current, [ownerChannelId]: value }
    setSeekDrafts(seekDraftsRef.current)
  }

  const clearSeekDraft = (ownerChannelId: number) => {
    const next = { ...seekDraftsRef.current }
    delete next[ownerChannelId]
    seekDraftsRef.current = next
    setSeekDrafts(next)
  }

  const commitSeek = async (source: FilePlaybackSource) => {
    const owner = source.owner_channel_id
    const requested = seekDraftsRef.current[owner]
    if (!fileTimelineEnabled || requested === undefined || seekBusyRef.current.has(owner)) return
    seekBusyRef.current.add(owner)
    setSeekBusy(previous => ({ ...previous, [owner]: true }))
    try {
      const response = await seekFilePlayback(appName, owner, requested)
      clearSeekDraft(owner)
      setFileSources(previous => previous.map(item => item.owner_channel_id === owner
        ? { ...item, position_ms: response.position_ms, ended: false, playing: true }
        : item))
    } catch (error) {
      clearSeekDraft(owner)
      showToast(`视频跳转失败：${errMsg(error)}`, 'err')
    } finally {
      seekBusyRef.current.delete(owner)
      setSeekBusy(previous => ({ ...previous, [owner]: false }))
    }
  }

  // 页面始终跟随当前唯一处于 running 状态的视觉程序。
  useEffect(() => {
    let disposed = false
    let requestPending = false
    const loadApps = async () => {
      if (requestPending) return
      requestPending = true
      try {
        const data = await fetchApps()
        if (!disposed) {
          setApps(data)
          setAppsError('')
        }
      } catch (error) {
        if (!disposed) setAppsError(`读取程序状态失败：${errMsg(error)}`)
      } finally {
        requestPending = false
        if (!disposed) setAppsLoading(false)
      }
    }
    loadApps()
    const timer = setInterval(loadApps, 1000)
    return () => {
      disposed = true
      clearInterval(timer)
    }
  }, [])

  // 读取程序真正使用的运行配置。未明确启用 RTSP 时不创建 img 流连接。
  useEffect(() => {
    if (!appName) {
      setRtspState('idle')
      setVideoLayout(null)
      return
    }
    let disposed = false
    setRtspState('checking')
    setVideoLayout(null)
    const checkRtsp = async () => {
      try {
        const path = runtimeConfigPath(runningConfig)
        const config = path ? await loadConfigFile(appName, path) : await fetchConfig(appName)
        if (disposed) return
        if (!config) {
          setRtspState('error')
          return
        }
        setVideoLayout(videoTimelineLayout(config as Record<string, unknown>))
        const globalConfig = ((config as Record<string, unknown>).global ?? config) as Record<string, unknown>
        setRtspState(Number(globalConfig.enable_rtsp ?? 0) !== 0 ? 'enabled' : 'disabled')
      } catch {
        if (!disposed) setRtspState('error')
      }
    }
    checkRtsp()
    return () => { disposed = true }
  }, [appName, runningConfig, runningApp?.pid])

  // 通道/全局动作定义和控制 socket 状态会在程序刚启动后变化，因此持续刷新。
  useEffect(() => {
    if (!appName) {
      setControls(null)
      setActionBusy({})
      return
    }
    let disposed = false
    const loadControls = async () => {
      try {
        const data = await fetchLogicControls(appName)
        if (!disposed) setControls(data)
      } catch {
        if (!disposed) setControls(null)
      }
    }
    setControls(null)
    setActionBusy({})
    loadControls()
    const timer = setInterval(loadControls, 1000)
    return () => {
      disposed = true
      clearInterval(timer)
    }
  }, [appName])

  // 画面真正进入 playing 后再读取文件时间轴；加载、重连和出错时暂停进度条功能。
  useEffect(() => {
    if (!fileTimelineEnabled) {
      setFileSources([])
      seekDraftsRef.current = {}
      setSeekDrafts({})
      return
    }
    let disposed = false
    let requestPending = false
    const loadPlayback = async () => {
      if (requestPending) return
      requestPending = true
      try {
        const response = await fetchFilePlayback(appName)
        if (!disposed) setFileSources(Array.isArray(response.sources) ? response.sources : [])
      } catch {
        if (!disposed) setFileSources([])
      } finally {
        requestPending = false
      }
    }
    loadPlayback()
    const timer = setInterval(loadPlayback, 750)
    return () => {
      disposed = true
      clearInterval(timer)
    }
  }, [appName, fileTimelineEnabled])

  // 只有运行配置明确开启 RTSP 时才开始拉流；程序停止或关闭 RTSP 会立即清理重试。
  useEffect(() => {
    clearStreamRetry()
    streamRetryRef.current = 0
    setStreamRetryWaitMs(0)
    streamLastErrorRef.current = ''
    streamRetryDiagnosticsRef.current = {}
    setStreamErr(false)
    setStreamErrorDetail('')
    setStreamLoading(true)
    setSnapshotReady(false)
    return clearStreamRetry
  }, [appName, rtspEnabled])

  useEffect(() => {
    const syncFullscreenState = () => {
      setVideoFullscreen(document.fullscreenElement === videoFrameRef.current)
    }
    document.addEventListener('fullscreenchange', syncFullscreenState)
    return () => document.removeEventListener('fullscreenchange', syncFullscreenState)
  }, [])

  // H264 fMP4 通过 MSE 直接交给浏览器硬解。此处只维护增量缓冲和实时点，
  // 不在前端解析视频帧，也不保留 MJPEG/图片播放回退。
  useEffect(() => {
    if (!appName || !rtspEnabled || streamErr) return
    const video = videoRef.current
    if (!video) return
    if (!('MediaSource' in window)) {
      setStreamLoading(false)
      setStreamErr(true)
      showToast('当前浏览器不支持 Media Source Extensions，无法进行 H264 零转码播放', 'err')
      return
    }

    let disposed = false
    const abortController = new AbortController()
    const mediaSource = new MediaSource()
    const objectUrl = URL.createObjectURL(mediaSource)
    let sourceBuffer: SourceBuffer | null = null
    let reader: ReadableStreamDefaultReader<Uint8Array> | null = null

    video.src = objectUrl
    video.muted = true
    let playbackStarted = false
    const playbackBuffer = new LivePlaybackBuffer()
    let lastTrimAt = 0
    video.playbackRate = 1
    const syncLivePosition = () => {
      if (!sourceBuffer || disposed || video.seeking || sourceBuffer.updating || !sourceBuffer.buffered.length) return
      const last = sourceBuffer.buffered.length - 1
      const end = sourceBuffer.buffered.end(last)
      if (playbackBuffer.rebuffering) {
        if (playbackBuffer.resume(end - video.currentTime)) void video.play().catch(() => {})
        return
      }
      const target = livePlaybackTarget(video.currentTime, sourceBuffer.buffered.start(last), end, playbackStarted, playbackBuffer.targetSeconds)
      if (target !== null) {
        video.currentTime = target
        if (!playbackStarted) void video.play().catch(() => {})
      }
      video.playbackRate = playbackStarted ? livePlaybackRate(video.currentTime, end, playbackBuffer.targetSeconds) : 1
    }

    const append = async (bytes: Uint8Array) => {
      if (!sourceBuffer || mediaSource.readyState !== 'open' || disposed) return

      syncLivePosition()
      // 留足完整 GOP 和短暂抖动余量；清理不跟随每个网络包重复执行。
      if (sourceBuffer.buffered.length > 0) {
        const bufferedStart = sourceBuffer.buffered.start(0)
        const keepFrom = video.currentTime - 8
        if (performance.now() - lastTrimAt > 2000 && video.currentTime - bufferedStart > 12 && keepFrom > bufferedStart) {
          await sourceBufferOperation(sourceBuffer, () => sourceBuffer!.remove(bufferedStart, keepFrom), abortController.signal)
          lastTrimAt = performance.now()
        }
      }
      try {
        await sourceBufferOperation(sourceBuffer, () => sourceBuffer!.appendBuffer(sourceBufferBytes(bytes)), abortController.signal)
      } catch (error) {
        if (!(error instanceof DOMException) || error.name !== 'QuotaExceededError' || disposed ||
            !sourceBuffer.buffered.length || video.currentTime - sourceBuffer.buffered.start(0) <= 2) throw error
        // 配额不足先释放已播放的数据，重试同一包，避免把可恢复的内存压力变成黑屏重连。
        await sourceBufferOperation(sourceBuffer, () => sourceBuffer!.remove(sourceBuffer!.buffered.start(0), video.currentTime - 2), abortController.signal)
        await sourceBufferOperation(sourceBuffer, () => sourceBuffer!.appendBuffer(sourceBufferBytes(bytes)), abortController.signal)
      }

      // 正常播放保持原速；仅在严重积压或时间线断层时跳到实时点。
      syncLivePosition()
      if (playbackStarted && video.paused && !playbackBuffer.rebuffering) void video.play().catch(() => {})
    }

    const start = async () => {
      await waitForSourceOpen(mediaSource, abortController.signal)
      const token = useAuthStore.getState().token ?? ''
      const query = new URLSearchParams({ t: String(streamNonce), client_id: streamClientId, ...streamRetryDiagnosticsRef.current })
      const response = await fetch(`${streamUrl(appName)}?${query}`, {
        headers: token ? { Authorization: `Bearer ${token}` } : undefined,
        cache: 'no-store',
        signal: abortController.signal,
      })
      if (!response.ok) {
        let detail = `视频服务返回 HTTP ${response.status}`
        try {
          const body = await response.json() as { detail?: string }
          if (body.detail) detail = body.detail
        } catch { /* 非 JSON 错误响应 */ }
        if (response.status === 409) throw new FatalStreamError(detail)
        throw new Error(detail)
      }
      if (!response.body) throw new Error('浏览器没有收到视频响应体')
      reader = response.body.getReader()

      // SourceBuffer 必须使用视频真实的 AVC codec string；从初始化段 avcC 中读取，
      // 避免把摄像头的 Main/High Profile 错报成固定 Baseline。
      const initialParts: Uint8Array[] = []
      let initialSize = 0
      let codec: string | null = null
      let initialBytes: Uint8Array | null = null
      while (!codec) {
        const result = await readStreamChunk(reader, STREAM_STALL_MS, abortController.signal)
        if (result.done || !result.value) throw new Error('视频流在初始化完成前中断')
        initialParts.push(result.value)
        initialSize += result.value.byteLength
        if (initialSize > STREAM_INIT_LIMIT) throw new Error('视频初始化段异常：未找到 H264 avcC')
        initialBytes = concatBytes(initialParts, initialSize)
        codec = findAvcCodec(initialBytes)
      }

      const mime = `video/mp4; codecs="${codec}"`
      if (!MediaSource.isTypeSupported(mime)) {
        throw new FatalStreamError(`当前浏览器不支持该 H264 格式：${codec}`)
      }
      sourceBuffer = mediaSource.addSourceBuffer(mime)
      sourceBuffer.mode = 'segments'
      await append(initialBytes!)

      while (!disposed) {
        const result = await readStreamChunk(reader, STREAM_STALL_MS, abortController.signal)
        if (result.done) throw new Error('视频流已中断')
        if (result.value?.byteLength) await append(result.value)
      }
    }

    start().catch(error => {
      if (disposed || abortController.signal.aborted) return
      setStreamLoading(false)
      if (error instanceof FatalStreamError) {
        setStreamErr(true)
        setStreamErrorDetail(error.message)
        showToast(error.message, 'err')
      } else {
        const reason = errMsg(error)
        const cause: StreamRetryCause = error instanceof StreamOperationTimeout
          ? error.phase === 'read' ? 'read-timeout' : error.phase === 'buffer' ? 'buffer-timeout' : 'initialization-timeout'
          : 'transport-error'
        scheduleStreamRetry(reason, cause)
        abortController.abort()
      }
    })

    const onVideoError = () => {
      if (!disposed) {
        scheduleStreamRetry(mediaErrorMessage(video.error), 'decoder-error')
        abortController.abort()
      }
    }
    const onVideoPlaying = () => { playbackStarted = true }
    const onVideoWaiting = () => {
      if (disposed || !playbackStarted || video.seeking || document.hidden || !video.buffered.length) return
      const ahead = video.buffered.end(video.buffered.length - 1) - video.currentTime
      if (playbackBuffer.starved(ahead)) {
        video.playbackRate = 1
        video.pause()
      }
    }
    video.addEventListener('error', onVideoError)
    video.addEventListener('playing', onVideoPlaying)
    video.addEventListener('waiting', onVideoWaiting)
    const startupTimer = setTimeout(() => {
      if (!disposed && !playbackStarted) {
        scheduleStreamRetry('视频数据已连接，但浏览器在 15 秒内未能开始播放', 'startup-timeout')
        abortController.abort()
      }
    }, STREAM_STALL_MS)
    let frameRequest: number | null = null
    let presentedFrameCount = 0
    const hasFrameCallback = typeof video.requestVideoFrameCallback === 'function'
    const onPresentedFrame = () => {
      if (disposed) return
      presentedFrameCount += 1
      frameRequest = video.requestVideoFrameCallback(onPresentedFrame)
    }
    if (hasFrameCallback) frameRequest = video.requestVideoFrameCallback(onPresentedFrame)
    const presentedFrames = (): number | null => {
      if (hasFrameCallback) return presentedFrameCount
      if (typeof video.getVideoPlaybackQuality !== 'function') return null
      const quality = video.getVideoPlaybackQuality()
      return quality.totalVideoFrames - quality.droppedVideoFrames
    }
    const progress = new PlaybackProgress(performance.now(), video.currentTime, presentedFrames(), STREAM_STALL_MS)
    const playbackWatchdog = setInterval(() => {
      if (disposed || document.hidden || !playbackStarted) {
        progress.reset(performance.now(), video.currentTime, presentedFrames())
        return
      }
      syncLivePosition()
      const health = progress.sample(performance.now(), video.currentTime, presentedFrames(), video.seeking, video.paused)
      // A single playing event is insufficient: only sustained frame presentation
      // resets backoff, so flapping sources cannot create a rapid retry loop.
      if (health.stable) streamRetryRef.current = 0
      if (health.stalled) {
        scheduleStreamRetry('实时画面已停滞超过 15 秒', 'playback-stall')
        abortController.abort()
      }
    }, 250)
    const onVisibilityChange = () => {
      progress.reset(performance.now(), video.currentTime, presentedFrames())
      if (document.hidden) return
      // 回到前台优先复用解码器和连接；积压由缓冲同步处理，避免切换窗口必然黑屏。
      syncLivePosition()
      if (playbackStarted && video.paused && !playbackBuffer.rebuffering) void video.play().catch(() => {})
    }
    document.addEventListener('visibilitychange', onVisibilityChange)

    return () => {
      disposed = true
      clearTimeout(startupTimer)
      clearInterval(playbackWatchdog)
      if (frameRequest !== null && typeof video.cancelVideoFrameCallback === 'function') video.cancelVideoFrameCallback(frameRequest)
      document.removeEventListener('visibilitychange', onVisibilityChange)
      abortController.abort()
      reader?.cancel().catch(() => {})
      video.removeEventListener('error', onVideoError)
      video.removeEventListener('playing', onVideoPlaying)
      video.removeEventListener('waiting', onVideoWaiting)
      try {
        if (mediaSource.readyState === 'open') mediaSource.endOfStream()
      } catch { /* 流已关闭 */ }
      video.pause()
      video.removeAttribute('src')
      video.load()
      URL.revokeObjectURL(objectUrl)
    }
  }, [appName, rtspEnabled, streamNonce, streamErr]) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => () => {
    clearStreamRetry()
    if (toastTimerRef.current) clearTimeout(toastTimerRef.current)
  }, [])

  const statusText = !runningApp
    ? '未运行'
    : rtspState === 'checking'
      ? '检测配置中'
      : rtspState === 'disabled'
        ? 'RTSP 未开启'
        : rtspState === 'error'
          ? '配置读取失败'
          : streamErr
            ? '视频异常'
            : streamLoading
              ? '正在连接'
              : '实时'

  const statusClass = runningApp && rtspEnabled && !streamErr
    ? (streamLoading ? 'pending' : 'online')
    : runningApp ? 'warning' : 'offline'

  return (
    <div className="live-view-page">
      <header className="live-view-page-header">
        <div>
          <h2>实时画面</h2>
        </div>
        <div className="live-view-runtime">
          {runningApp && <span className="live-view-app-name">{runningApp.name}</span>}
          <span className={`live-view-status ${statusClass}`}>{statusText}</span>
        </div>
      </header>

      {toast && (
        <div className={`live-view-toast ${toast.type}`}>
          <span>{toast.msg}</span>
          {toast.type === 'err' && <button onClick={() => setToast(null)}>×</button>}
        </div>
      )}

      {appsLoading ? (
        <div className="live-view-empty">
          <div className="live-view-empty-icon">◌</div>
          <h3>正在读取程序状态……</h3>
        </div>
      ) : !runningApp ? (
        <div className="live-view-empty">
          <div className="live-view-empty-icon">▶</div>
          <h3>当前没有正在运行的视觉程序</h3>
          {appsError && <p className="live-view-empty-error">{appsError}</p>}
        </div>
      ) : (
        <div
          className="live-view-workspace"
          ref={workspaceRef}
          style={{ '--live-view-side-width': `${sidePanelWidth}px` } as React.CSSProperties}
        >
          <section className={`live-view-visual-panel${videoFullscreen ? ' fullscreen-active' : ''}`}>
            <div
              className="live-view-video-frame"
              ref={videoFrameRef}
              tabIndex={timelineTiles.length > 0 ? 0 : undefined}
              onFocus={focusTimeline}
              onKeyDown={selectTimelineByKeyboard}
            >
              <button
                className="live-view-fullscreen-btn"
                onClick={toggleVideoFullscreen}
                title={videoFullscreen ? '退出全屏（Esc）' : '全屏显示视频'}
              >
                <span aria-hidden="true">{videoFullscreen ? '⊠' : '⛶'}</span>
                {videoFullscreen ? '退出全屏' : '全屏'}
              </button>
              <div className="live-view-video-stage">
                {rtspEnabled ? (
                  !streamErr ? (
                    <>
                      <video
                        ref={videoRef}
                        key={`${appName}-${streamNonce}`}
                        className="live-view-video"
                        style={streamLoading && !snapshotReady ? { visibility: 'hidden' } : undefined}
                        muted
                        autoPlay
                        playsInline
                        onPlaying={handleStreamLoad}
                      />
                      <canvas ref={videoSnapshotRef} className="live-view-video-snapshot" hidden={!streamLoading || !snapshotReady} />
                      {streamLoading && (
                        <div className={`live-view-loading${snapshotReady ? ' reconnecting' : ''}`}>
                          <div className="live-view-spinner" />
                          <span>{snapshotReady ? '连接恢复中，暂时显示上一帧……' : '正在准备实时画面……'}</span>
                          <div
                            className="live-view-progress"
                            role="progressbar"
                            aria-label="实时画面连接进度"
                            aria-valuetext="正在连接视频流"
                          >
                            <div className="live-view-progress-fill" />
                          </div>
                          <span className="live-view-progress-text">
                            {streamRetryWaitMs > 0 ? `${streamErrorDetail}；约 ${Math.ceil(streamRetryWaitMs / 1000)} 秒后重试`
                              : streamErrorDetail || '正在连接视频流，通常约 3 秒'}
                          </span>
                        </div>
                      )}
                    </>
                  ) : (
                    <div className="live-view-video-state error">
                      <strong>实时视频暂不可用</strong>
                      <span>{streamErrorDetail || '请检查程序推流状态和视频源连接。'}</span>
                      <button onClick={retryStream}>重新连接</button>
                    </div>
                  )
                ) : (
                  <div className="live-view-video-state">
                    <strong>
                      {rtspState === 'checking'
                        ? '正在检查运行配置'
                        : rtspState === 'disabled'
                          ? '当前程序未开启 RTSP 推流'
                          : '无法确认 RTSP 推流配置'}
                    </strong>
                  </div>
                )}
              </div>
              {timelineTiles.length > 0 && videoLayout && (
                <div
                  className="live-view-file-timelines"
                  style={{
                    '--picture-left': `${pictureBounds.left}px`,
                    '--picture-top': `${pictureBounds.top}px`,
                    '--picture-width': pictureBounds.width > 0 ? `${pictureBounds.width}px` : '100%',
                    '--picture-height': `${pictureBounds.height}px`,
                  } as React.CSSProperties}
                >
                  {timelineTiles.map(tile => {
                    const { source, channelId } = tile
                    const owner = source.owner_channel_id
                    const value = seekDrafts[owner] ?? source.position_ms
                    const duration = Math.max(0, source.duration_ms)
                    const disabled = !source.available || !source.seekable || duration <= 0 || !!seekBusy[owner]
                    const sharedChannels = source.channel_ids.filter(id => id !== channelId)
                    return (
                      <div
                        className={`live-view-file-timeline${activeTimelineChannel === channelId ? '' : ' is-hidden'}`}
                        key={channelId}
                        data-timeline-channel={channelId}
                        aria-hidden={activeTimelineChannel !== channelId}
                        style={{
                          '--tile-left': `${tile.left * 100}%`,
                          '--tile-width': `${tile.width * 100}%`,
                          '--tile-bottom': `${(1 - tile.top - tile.height) * 100}%`,
                        } as React.CSSProperties}
                        onPointerDownCapture={event => {
                          if (!event.isPrimary) return
                          fileTimelinePointerRef.current = { pointerId: event.pointerId, channelId }
                          showFileTimeline(channelId)
                        }}
                        onFocusCapture={() => showFileTimeline(channelId)}
                        onKeyDownCapture={() => showFileTimeline(channelId)}
                      >
                        <div className="live-view-file-meta">
                          <span title={sharedChannels.length > 0
                            ? `通道 ${channelId} · 与通道 ${sharedChannels.join('、')} 共用进度 · ${source.location}`
                            : source.location}>
                            通道 {channelId} · {sourceFileName(source.location)}
                            {sharedChannels.length > 0 ? ' · 共用进度' : ''}
                            {source.ended ? ' · 已播完' : ''}
                          </span>
                          <span className="live-view-file-time">
                            {formatPlaybackTime(value)} / {formatPlaybackTime(duration)}
                          </span>
                        </div>
                        <input
                          type="range"
                          min={0}
                          max={Math.max(1, duration)}
                          step={100}
                          value={Math.min(value, Math.max(1, duration))}
                          disabled={disabled}
                          aria-label={`通道 ${channelId} ${sourceFileName(source.location)} 播放进度`}
                          onChange={event => setSeekDraft(owner, Number(event.target.value))}
                          onPointerUp={() => { void commitSeek(source) }}
                          onKeyUp={event => {
                            if (['ArrowLeft', 'ArrowRight', 'Home', 'End', 'PageUp', 'PageDown'].includes(event.key)) {
                              void commitSeek(source)
                            }
                          }}
                          onBlur={() => { void commitSeek(source) }}
                        />
                      </div>
                    )
                  })}
                </div>
              )}
            </div>
          </section>

          <div className="live-view-side-column">
            <div
              className="live-view-side-resizer"
              role="separator"
              aria-label="调整实时画面右侧栏宽度"
              aria-orientation="vertical"
              aria-valuemin={LIVE_SIDE_WIDTH_MIN}
              aria-valuemax={LIVE_SIDE_WIDTH_MAX}
              aria-valuenow={Math.round(sidePanelWidth)}
              tabIndex={0}
              title="左右拖拽调整右侧栏宽度"
              onPointerDown={beginSideResize}
              onPointerMove={moveSideResize}
              onPointerUp={endSideResize}
              onPointerCancel={endSideResize}
              onLostPointerCapture={endSideResize}
              onKeyDown={resizeSideByKeyboard}
            />
            <LiveLogConsole key={appName} appName={appName} />

            <section className="live-view-controls-panel">
              <div className="live-view-panel-head">
                <span>自定义功能</span>
                {controls && !controls.socket_ready && (
                  <span className="live-view-control-badge">控制通道未连接</span>
                )}
              </div>
              <div className="live-view-controls">
                {!controls ? (
                  <div className="live-view-control-empty">暂时无法获取通道控制信息。</div>
                ) : !hasActions ? (
                  <div className="live-view-control-empty">当前程序没有可用的自定义功能。</div>
                ) : (
                  <>
                    <div className="live-view-control-grid">
                      {actionTargets.filter(target => target.actions.length > 0).map(target => (
                        <div key={target.key} className="live-view-control-card">
                          <div className="live-view-control-title">
                            <span>{target.title}</span>
                            <span>{target.subtitle}</span>
                          </div>
                          <div className="live-view-control-actions">
                            {target.actions.map(action => {
                              const key = `${target.key}:${action.id}`
                              const disabled = !controls.socket_ready || !target.enabled || !!actionBusy[key]
                              return (
                                <button
                                  key={key}
                                  className={`live-view-action ${action.style ?? 'default'}`}
                                  disabled={disabled}
                                  title={action.help ?? action.id}
                                  onClick={() => handleAction(
                                    target.key,
                                    target.title,
                                    action,
                                    () => target.run(action),
                                  )}
                                >
                                  {actionBusy[key] ? '处理中…' : (action.label ?? action.id)}
                                </button>
                              )
                            })}
                          </div>
                        </div>
                      ))}
                    </div>
                  </>
                )}
              </div>
            </section>
          </div>
        </div>
      )}
    </div>
  )
}
