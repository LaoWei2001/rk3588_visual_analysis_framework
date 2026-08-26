import { useCallback, useEffect, useMemo, useState } from 'react'
import {
  applyCameraConfiguration,
  CameraConfiguration,
  CameraConfigurationInput,
  CameraInterfaceInfo,
  CameraNetworkPlan,
  CameraSettingsSnapshot,
  CameraStatusInfo,
  discoverCameras,
  DiscoveredCameraInfo,
  fetchCameraSettings,
  fetchCameraStatus,
  openCameraWebProxy,
  planCameraConfiguration,
  removeCameraConfiguration,
} from '../api/client'
import './CameraSettingsPage.css'

type Busy = 'load' | 'discover' | 'apply' | 'remove' | 'status' | 'web' | null
type Notice = { type: 'ok' | 'err'; text: string } | null

const errorText = (error: unknown): string => {
  const value = error as { response?: { data?: { detail?: string } }; message?: string }
  return value.response?.data?.detail || value.message || String(error)
}

const statusLabel = (ready: boolean, positive = '正常', negative = '不可用') =>
  ready ? positive : negative

export default function CameraSettingsPage() {
  const [snapshot, setSnapshot] = useState<CameraSettingsSnapshot | null>(null)
  const [status, setStatus] = useState<CameraStatusInfo | null>(null)
  const [device, setDevice] = useState('')
  const [cameras, setCameras] = useState<DiscoveredCameraInfo[]>([])
  const [cameraIp, setCameraIp] = useState('')
  const [prefixLength, setPrefixLength] = useState('')
  const [cameraMac, setCameraMac] = useState('')
  const [model, setModel] = useState('')
  const [serial, setSerial] = useState('')
  const [httpPort, setHttpPort] = useState('80')
  const [rtspPort, setRtspPort] = useState('554')
  const [httpInferred, setHttpInferred] = useState(false)
  const [rtspInferred, setRtspInferred] = useState(false)
  const [plan, setPlan] = useState<CameraNetworkPlan | null>(null)
  const [busy, setBusy] = useState<Busy>('load')
  const [notice, setNotice] = useState<Notice>(null)

  const fillConfiguration = useCallback((configuration: CameraConfiguration) => {
    setDevice(configuration.interface)
    setCameraIp(configuration.camera_ip)
    setPrefixLength(String(configuration.camera_prefix_length))
    setCameraMac(configuration.camera_mac || '')
    setModel(configuration.model || '')
    setSerial(configuration.serial || '')
    setHttpPort(String(configuration.http_port))
    setRtspPort(String(configuration.rtsp_port))
    setHttpInferred(configuration.http_port_inferred)
    setRtspInferred(configuration.rtsp_port_inferred)
    setPlan(configuration)
  }, [])

  const load = useCallback(async (resetForm = false) => {
    setBusy('load')
    try {
      const value = await fetchCameraSettings(true)
      setSnapshot(value)
      setStatus(value.status)
      if (resetForm && value.configuration) fillConfiguration(value.configuration)
      if (!device) setDevice(value.configuration?.interface || value.interfaces[0]?.device || '')
    } catch (error) {
      setNotice({ type: 'err', text: errorText(error) })
    } finally {
      setBusy(null)
    }
  }, [device, fillConfiguration])

  useEffect(() => { void load(true) }, []) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (!snapshot?.configuration) return
    const timer = window.setInterval(async () => {
      try {
        const value = await fetchCameraStatus()
        setStatus(value.status)
      } catch { /* 页面上的手动刷新会显示详细错误，轮询静默失败 */ }
    }, 8000)
    return () => window.clearInterval(timer)
  }, [snapshot?.configuration])

  const selectedInterface = useMemo(
    () => snapshot?.interfaces.find(item => item.device === device) || null,
    [snapshot, device],
  )

  const clearPlan = () => setPlan(null)

  const chooseCamera = (camera: DiscoveredCameraInfo) => {
    setCameraIp(camera.ip)
    setPrefixLength(camera.prefix_length == null ? '' : String(camera.prefix_length))
    setCameraMac(camera.mac)
    setModel(camera.model)
    setSerial(camera.serial)
    setHttpPort(String(camera.http_port))
    setRtspPort(String(camera.rtsp_port))
    setHttpInferred(camera.http_port_inferred)
    setRtspInferred(camera.rtsp_port_inferred)
    setPlan(null)
  }

  const search = async () => {
    if (!device) return
    setBusy('discover'); setNotice(null); setCameras([])
    try {
      const value = await discoverCameras(device)
      setCameras(value.cameras)
      if (value.cameras.length === 1) chooseCamera(value.cameras[0])
      setNotice(value.cameras.length
        ? { type: 'ok', text: `在 ${device} 上发现 ${value.cameras.length} 台摄像头。` }
        : { type: 'err', text: `在 ${device} 上未收到摄像头响应，请检查网线、供电及设备发现功能。` })
    } catch (error) {
      setNotice({ type: 'err', text: errorText(error) })
    } finally {
      setBusy(null)
    }
  }

  const formValue = (): CameraConfigurationInput => {
    const prefix = Number(prefixLength)
    const http = Number(httpPort)
    const rtsp = Number(rtspPort)
    if (!device) throw new Error('请选择摄像头连接的物理网口')
    if (!cameraIp.trim()) throw new Error('请填写或搜索得到摄像头 IP')
    if (!Number.isInteger(prefix) || prefix < 1 || prefix > 30) {
      throw new Error('请填写摄像头真实子网前缀（1–30）；自动发现通常会直接给出')
    }
    if (!Number.isInteger(http) || http < 1 || http > 65535 ||
        !Number.isInteger(rtsp) || rtsp < 1 || rtsp > 65535) {
      throw new Error('HTTP/RTSP 端口必须在 1–65535 之间')
    }
    return {
      interface: device,
      camera_ip: cameraIp.trim(),
      prefix_length: prefix,
      camera_mac: cameraMac,
      model,
      serial,
      http_port: http,
      rtsp_port: rtsp,
      http_port_inferred: httpInferred,
      rtsp_port_inferred: rtspInferred,
    }
  }

  const apply = async () => {
    setNotice(null)
    let input: CameraConfigurationInput
    try { input = formValue() } catch (error) {
      setNotice({ type: 'err', text: errorText(error) }); return
    }
    setBusy('apply')
    try {
      const checked = await planCameraConfiguration(input)
      setPlan(checked)
      const conflictText = checked.conflicts.length
        ? `\n检测到 ${checked.conflicts.length} 个重叠网段，将用 /32 主机路由隔离。`
        : ''
      const warningText = checked.warnings.length ? `\n\n${checked.warnings.join('\n')}` : ''
      const confirmed = window.confirm(
        `将在 ${checked.interface} 上使用本地地址 ${checked.local_ip}/32，` +
        `并仅添加到 ${checked.camera_ip}/32 的路由。不会设置网关、DNS或默认路由。` +
        conflictText + warningText + '\n\n确定应用并持久化？',
      )
      if (!confirmed) return
      const value = await applyCameraConfiguration(input)
      setStatus(value.status)
      setSnapshot(previous => previous ? {
        ...previous,
        configuration: value.configuration,
        status: value.status,
        status_error: value.status_error,
      } : previous)
      fillConfiguration(value.configuration)
      setNotice({
        type: 'ok',
        text: value.status_error
          ? `网络配置已应用并持久化；状态检测提示：${value.status_error}`
          : '摄像头网络配置已应用并持久化，重启后会自动恢复。',
      })
      await load(false)
    } catch (error) {
      setNotice({ type: 'err', text: errorText(error) })
    } finally {
      setBusy(null)
    }
  }

  const remove = async () => {
    if (!window.confirm('确定移除摄像头持久化配置及由本页面创建的地址/路由？原有网络资源不会删除。')) return
    setBusy('remove'); setNotice(null)
    try {
      await removeCameraConfiguration()
      setSnapshot(previous => previous ? { ...previous, configuration: null, status: null } : previous)
      setStatus(null); setPlan(null)
      setNotice({ type: 'ok', text: '摄像头配置已移除；未触碰预先存在的地址和路由。' })
      await load(false)
    } catch (error) {
      setNotice({ type: 'err', text: errorText(error) })
    } finally {
      setBusy(null)
    }
  }

  const refreshStatus = async () => {
    setBusy('status'); setNotice(null)
    try {
      const value = await fetchCameraStatus()
      setStatus(value.status)
    } catch (error) {
      setNotice({ type: 'err', text: errorText(error) })
    } finally {
      setBusy(null)
    }
  }

  const openWeb = async () => {
    if (!snapshot?.configuration) {
      setNotice({ type: 'err', text: '请先应用并持久化摄像头网络配置' }); return
    }
    // 必须在用户点击的同步调用中创建窗口，否则浏览器可能把异步打开判定为弹窗。
    const popup = window.open('about:blank', '_blank')
    if (!popup) {
      setNotice({ type: 'err', text: '浏览器阻止了新窗口，请允许本站弹窗后重试' }); return
    }
    popup.opener = null
    setBusy('web'); setNotice(null)
    try {
      const session = await openCameraWebProxy()
      const hostname = window.location.hostname
      const host = hostname.includes(':') ? `[${hostname}]` : hostname
      const url = `http://${host}:${session.port}/`
      popup.location.replace(url)
      setNotice({
        type: 'ok',
        text: `已为当前 Windows/浏览器客户端临时开放 RK3588 摄像头代理端口 ${session.port}（30 分钟）。`,
      })
    } catch (error) {
      popup.close()
      setNotice({ type: 'err', text: errorText(error) })
    } finally {
      setBusy(null)
    }
  }

  return (
    <div className="camera-settings-page">
      <header className="camera-settings-header">
        <div><h2>摄像头配置</h2><p>管理通过独立有线网口直连 RK3588 的海康网络摄像头。</p></div>
        <button className="camera-ghost-btn" disabled={busy !== null} onClick={() => void load(false)}>
          {busy === 'load' ? '刷新中…' : '刷新网口'}
        </button>
      </header>

      {notice && <div className={`camera-notice ${notice.type}`}>{notice.text}</div>}
      {snapshot?.error && <div className="camera-notice err">{snapshot.error}</div>}

      <section className="camera-card">
        <div className="camera-card-head">
          <div><h3>1. 选择摄像头网口</h3><p>自动枚举物理有线网口，不依赖 eth1 等固定名称。</p></div>
        </div>
        {!snapshot ? <div className="camera-empty">正在读取物理网口…</div> :
          snapshot.interfaces.length === 0 ? <div className="camera-empty">没有检测到可用的物理有线网口。</div> :
          <div className="camera-interface-grid">
            {snapshot.interfaces.map(item =>
              <InterfaceCard key={item.device} item={item} active={device === item.device} onClick={() => {
                setDevice(item.device); setCameras([]); clearPlan()
              }} />)}
          </div>}
      </section>

      <section className="camera-card">
        <div className="camera-card-head">
          <div><h3>2. 搜索或填写摄像头</h3><p>SADP/ONVIF 搜索绑定到所选网口，冲突网段中的摄像头也能被发现。</p></div>
          <button className="camera-primary-btn" disabled={!device || busy !== null} onClick={search}>
            {busy === 'discover' ? '搜索中…' : '搜索摄像头'}
          </button>
        </div>
        {cameras.length > 0 && <div className="camera-discovery-list">
          {cameras.map(camera => <button key={`${camera.mac}-${camera.ip}`}
            className={cameraIp === camera.ip && cameraMac === camera.mac ? 'active' : ''}
            onClick={() => chooseCamera(camera)}>
            <span><b>{camera.model || '网络摄像头'}</b><small>{camera.ip} · {camera.mac || 'MAC 未广播'}</small></span>
            <em>{camera.hikvision ? '海康' : camera.source.toUpperCase()}</em>
          </button>)}
        </div>}
        <div className="camera-form-grid">
          <label><span>摄像头 IPv4</span><input value={cameraIp} placeholder="手动填写或搜索选择"
            onChange={event => { setCameraIp(event.target.value); setCameraMac(''); setModel(''); setSerial(''); clearPlan() }} /></label>
          <label><span>摄像头子网前缀</span><input type="number" min={1} max={30} value={prefixLength}
            placeholder="例如搜索结果中的 16/24" onChange={event => { setPrefixLength(event.target.value); clearPlan() }} /></label>
          <label><span>HTTP 端口</span><input type="number" min={1} max={65535} value={httpPort}
            onChange={event => { setHttpPort(event.target.value); setHttpInferred(false); clearPlan() }} />
            {httpInferred && <small>发现报文未明确提供，当前为推断值</small>}</label>
          <label><span>RTSP 端口</span><input type="number" min={1} max={65535} value={rtspPort}
            onChange={event => { setRtspPort(event.target.value); setRtspInferred(false); clearPlan() }} />
            {rtspInferred && <small>发现报文未明确提供，当前为推断值</small>}</label>
          <label><span>型号</span><input value={model} readOnly placeholder="搜索后显示" /></label>
          <label><span>摄像头 MAC</span><input value={cameraMac} readOnly placeholder="搜索后显示；手动 IP 可为空" /></label>
        </div>
      </section>

      <section className="camera-card">
        <div className="camera-card-head">
          <div><h3>3. 隔离规划与应用</h3><p>本地 IP 自动从摄像头真实网段选择；不配置摄像头网关，不修改 RK3588 默认路由。</p></div>
        </div>
        {plan ? <div className="camera-plan">
          <div><span>摄像头网段</span><b>{plan.camera_network}</b></div>
          <div><span>RK3588 摄像头侧</span><b>{plan.local_ip}/{plan.local_prefix_length}</b></div>
          <div><span>精确路由</span><b>{plan.camera_ip}/32 → {plan.interface}</b></div>
          <div><span>重叠网段</span><b className={plan.conflicts.length ? 'warn' : 'good'}>{plan.conflicts.length ? `${plan.conflicts.length} 个，已隔离` : '无'}</b></div>
          {plan.conflicts.length > 0 && <ul>{plan.conflicts.map(item =>
            <li key={`${item.interface}-${item.network}`}>{item.interface}: {item.address}（{item.network}）</li>)}</ul>}
          {plan.warnings.length > 0 && <div className="camera-plan-warnings">{plan.warnings.map(item => <p key={item}>{item}</p>)}</div>}
        </div> : <div className="camera-empty">点击“应用配置”后会先执行冲突检查并展示确认信息，再进行任何网络写入。</div>}
        <div className="camera-actions">
          {snapshot?.configuration && <button className="camera-ghost-btn danger" disabled={busy !== null} onClick={remove}>
            {busy === 'remove' ? '移除中…' : '移除配置'}
          </button>}
          <button className="camera-primary-btn" disabled={busy !== null || !selectedInterface || !cameraIp.trim()} onClick={apply}>
            {busy === 'apply' ? '检查并应用中…' : '应用配置'}
          </button>
        </div>
      </section>

      <section className="camera-card">
        <div className="camera-card-head">
          <div><h3>摄像头状态</h3><p>逐层区分网线、二层/IP、Web 和视频服务问题。</p></div>
          <div className="camera-head-actions">
            <button className="camera-ghost-btn" disabled={!snapshot?.configuration || busy !== null} onClick={refreshStatus}>
              {busy === 'status' ? '检测中…' : '刷新状态'}
            </button>
            <button className="camera-primary-btn" disabled={!snapshot?.configuration || busy !== null}
              onClick={() => void openWeb()}>
              {busy === 'web' ? '正在建立代理…' : '通过 RK3588 打开 Web 页面'}
            </button>
          </div>
        </div>
        {!snapshot?.configuration ? <div className="camera-empty">应用配置后开始状态检测。</div> :
          <div className="camera-status-grid">
            <StatusTile title="物理 Link" ready={!!status?.link_up} detail={statusLabel(!!status?.link_up, '已连接', '未连接')} />
            <StatusTile title="ARP / IP" ready={!!status?.arp_reachable}
              detail={status?.arp_reachable ? status.arp_mac || '已响应' : '无 ARP 响应'} warning={status?.mac_matches === false} />
            <StatusTile title="HTTP" ready={!!status?.http_reachable}
              detail={status?.http_reachable ? `HTTP ${status.http_status}` : '无有效 HTTP 响应'} />
            <StatusTile title="RTSP" ready={!!status?.rtsp_reachable}
              detail={status?.rtsp_reachable ? `RTSP ${status.rtsp_status}` : '无有效 RTSP 响应'} />
          </div>}
        {snapshot?.status_error && <div className="camera-inline-warning">{snapshot.status_error}</div>}
      </section>
    </div>
  )
}

function InterfaceCard({ item, active, onClick }: {
  item: CameraInterfaceInfo
  active: boolean
  onClick: () => void
}) {
  return <button className={`camera-interface ${active ? 'active' : ''}`} onClick={onClick}>
    <div><b>{item.device}</b><em className={item.link_up ? 'up' : ''}>{item.link_up ? 'Link Up' : 'Link Down'}</em></div>
    <strong>{item.speed_mbps == null ? '速率未知' : `${item.speed_mbps} Mbps`}</strong>
    <small>MAC {item.mac || '-'}</small>
    <small>{item.addresses.length ? item.addresses.join(' · ') : '尚无 IPv4'}</small>
    {item.has_default_route && <span>承载默认路由</span>}
    {item.configured && <span className="configured">当前摄像头网口</span>}
  </button>
}

function StatusTile({ title, ready, detail, warning = false }: {
  title: string
  ready: boolean
  detail: string
  warning?: boolean
}) {
  return <div className={`camera-status-tile ${ready ? 'ready' : ''} ${warning ? 'warning' : ''}`}>
    <span>{title}</span><b>{warning ? 'MAC 不匹配' : statusLabel(ready)}</b><small>{detail}</small>
  </div>
}
