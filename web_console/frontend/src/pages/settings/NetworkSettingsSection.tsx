import { useEffect, useMemo, useState } from 'react'
import {
  activateNetworkConnection,
  confirmNetworkTransaction,
  deleteNetworkConnection,
  fetchNetworkSettings,
  fetchNetworkTransaction,
  pingNetworkTarget,
  rollbackNetworkTransaction,
  saveDeviceHostname,
  scanWifiNetworks,
  startNetworkChange,
  type NetworkConnectionInfo,
  type NetworkInterfaceInfo,
  type NetworkSettings,
  type NetworkTransaction,
  type WifiNetworkInfo,
} from '../../api/client'

const err = (e: unknown) => (e as { response?: { data?: { detail?: string } } })?.response?.data?.detail || (e instanceof Error ? e.message : String(e))
const stateText = (value: string) => ({ connected: '已连接', disconnected: '未连接', unavailable: '不可用', unmanaged: '未托管', connecting: '连接中' }[value] || value)
const transactionText = (value: string) => ({
  scheduled: '等待切换', activating: '正在激活新网络', awaiting_confirmation: '等待确认',
  committing: '正在保存', rolling_back: '正在恢复原网络', confirmed: '已确认保存',
  rolled_back: '已恢复原网络', rollback_failed: '自动恢复失败',
}[value] || value)

type Busy = 'hostname' | 'network' | 'refresh' | 'wifi' | 'profile' | 'ping' | 'transaction' | null
const NETWORK_ROLLBACK_SECONDS = 60

export default function NetworkSettingsSection() {
  const [data, setData] = useState<NetworkSettings | null>(null)
  const [hostname, setHostname] = useState('')
  const [device, setDevice] = useState('')
  const [method, setMethod] = useState<'auto' | 'manual'>('auto')
  const [address, setAddress] = useState('')
  const [gateway, setGateway] = useState('')
  const [dns, setDns] = useState('')
  const [profileName, setProfileName] = useState('')
  const [ssid, setSsid] = useState('')
  const [wifiSecurity, setWifiSecurity] = useState<'wpa-psk' | 'sae' | 'open'>('wpa-psk')
  const [wifiPassword, setWifiPassword] = useState('')
  const [wifiNetworks, setWifiNetworks] = useState<WifiNetworkInfo[]>([])
  const [transaction, setTransaction] = useState<NetworkTransaction | null>(null)
  const [clock, setClock] = useState(() => Date.now())
  const [pingTarget, setPingTarget] = useState('')
  const [pingResult, setPingResult] = useState<string | null>(null)
  const [busy, setBusy] = useState<Busy>(null)
  const [message, setMessage] = useState<{ text: string; type: 'ok' | 'err' } | null>(null)

  const load = async (quiet = false) => {
    if (!quiet) setBusy('refresh')
    try {
      const value = await fetchNetworkSettings()
      setData(value)
      setHostname(value.hostname)
      if (value.pending_transaction) setTransaction(value.pending_transaction)
      setDevice(current => value.interfaces.some(item => item.device === current)
        ? current : value.interfaces.find(item => item.state === 'connected')?.device || value.interfaces[0]?.device || '')
    } catch (e) {
      if (!quiet) setMessage({ text: `读取网络信息失败：${err(e)}`, type: 'err' })
    } finally {
      if (!quiet) setBusy(null)
    }
  }

  useEffect(() => { void load() }, [])
  const selected = useMemo(() => data?.interfaces.find(item => item.device === device) || null, [data, device])

  useEffect(() => {
    if (!selected) return
    setMethod(selected.ipv4_method === 'manual' ? 'manual' : 'auto')
    setAddress(selected.addresses[0] || '')
    setGateway(selected.gateway || '')
    setDns(selected.dns.join(', '))
    setProfileName(selected.connection || '')
    if (selected.type === 'wifi') setSsid(selected.ssid || '')
    setWifiPassword('')
    setWifiNetworks([])
  }, [selected?.device, selected?.connection_uuid])

  useEffect(() => {
    if (!transaction || !['scheduled', 'activating', 'awaiting_confirmation', 'committing', 'rolling_back'].includes(transaction.status)) return
    const poll = window.setInterval(async () => {
      setClock(Date.now())
      try {
        const value = await fetchNetworkTransaction(transaction.id)
        setTransaction(value)
        if (['confirmed', 'rolled_back', 'rollback_failed'].includes(value.status)) void load(true)
      } catch {
        // 静态 IP 已切换时旧地址会失联，继续从新地址读取同一事务。
        const targetIp = transaction.target_addresses[0]?.split('/', 1)[0]
        if (!targetIp || targetIp === window.location.hostname) return
        const port = window.location.port ? `:${window.location.port}` : ''
        try {
          const value = await fetchNetworkTransaction(
            transaction.id, `${window.location.protocol}//${targetIp}${port}`,
          )
          setTransaction(value)
        } catch { /* systemd 回滚任务不依赖页面轮询。 */ }
      }
    }, 1500)
    return () => window.clearInterval(poll)
  }, [transaction?.id, transaction?.status, transaction?.target_addresses[0]])

  const remaining = transaction?.deadline
    ? Math.max(0, Math.ceil(transaction.deadline - clock / 1000))
    : 0

  const saveHostname = async () => {
    setBusy('hostname'); setMessage(null)
    try {
      const value = await saveDeviceHostname(hostname)
      setHostname(value.hostname)
      setData(current => current ? { ...current, hostname: value.hostname } : current)
      setMessage({ text: '设备名已更新', type: 'ok' })
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }

  const scanWifi = async () => {
    if (!selected || selected.type !== 'wifi') return
    setBusy('wifi'); setMessage(null)
    try {
      const value = await scanWifiNetworks(selected.device)
      setWifiNetworks(value.networks)
      if (!value.networks.length) setMessage({ text: '没有扫描到附近的 Wi-Fi', type: 'err' })
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }

  const chooseWifi = (network: WifiNetworkInfo) => {
    setSsid(network.ssid)
    if (network.ssid !== selected?.ssid) setProfileName(`Wi-Fi ${network.ssid}`)
    const security = network.security.toUpperCase()
    setWifiSecurity(security === 'OPEN' || security === '--' ? 'open' : security.includes('SAE') || security.includes('WPA3') ? 'sae' : 'wpa-psk')
    setWifiPassword('')
  }

  const applyNetwork = async () => {
    if (!selected) return
    if (selected.type === 'wifi' && !ssid.trim()) {
      setMessage({ text: '请先选择或填写 Wi-Fi SSID', type: 'err' }); return
    }
    const target = method === 'manual' ? address || '未填写' : 'DHCP 自动获取'
    const warning = `即将在 ${selected.device} 上试用新网络（${target}）。\n试用后需要使用新 IP 地址登录 Web 控制台并确认，${NETWORK_ROLLBACK_SECONDS} 秒内未确认将自动恢复原连接。确定继续？`
    if (!window.confirm(warning)) return
    setBusy('network'); setMessage(null)
    try {
      const sameWifi = selected.type !== 'wifi' || (!!selected.ssid && selected.ssid === ssid.trim())
      const value = await startNetworkChange({
        device: selected.device,
        type: selected.type,
        connection_uuid: sameWifi ? selected.connection_uuid : null,
        profile_name: selected.type === 'wifi' && !sameWifi && profileName === selected.connection
          ? '' : profileName.trim(),
        method,
        address,
        gateway,
        dns: dns.split(',').map(item => item.trim()).filter(Boolean),
        ssid: ssid.trim(),
        wifi_security: wifiSecurity,
        wifi_password: wifiPassword,
        rollback_seconds: NETWORK_ROLLBACK_SECONDS,
      })
      setWifiPassword('')
      setTransaction(value.transaction)
      setClock(Date.now())
      setMessage({ text: '安全切换已经开始。新网络通过检查后，必须在倒计时结束前确认。', type: 'ok' })
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }

  const confirmTransaction = async (baseUrl = '') => {
    if (!transaction) return
    setBusy('transaction'); setMessage(null)
    try {
      const value = await confirmNetworkTransaction(transaction.id, baseUrl)
      setTransaction(value)
      const savedText = transaction.kind === 'saved_profile'
        ? '已确认使用该连接配置。'
        : transaction.old_uuid
          ? '新网络已经确认并保存，原连接配置已作为禁用的备份保留。'
          : '新网络已经确认并保存。'
      setMessage({ text: savedText, type: 'ok' })
      await load(true)
    } catch (e) { setMessage({ text: `确认失败：${err(e)}`, type: 'err' }) }
    finally { setBusy(null) }
  }

  const rollbackTransaction = async () => {
    if (!transaction || !window.confirm('确定立即放弃新网络并恢复原连接？')) return
    setBusy('transaction'); setMessage(null)
    try {
      const value = await rollbackNetworkTransaction(transaction.id)
      setTransaction(value)
      setMessage({ text: value.status === 'rolled_back' ? '已经恢复原网络。' : value.error || '恢复原网络失败', type: value.status === 'rolled_back' ? 'ok' : 'err' })
      await load(true)
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }

  const activateProfile = async (profile: NetworkConnectionInfo) => {
    const targetDevice = selected?.type === profile.type
      ? selected.device
      : data?.interfaces.find(item => item.type === profile.type)?.device
    if (!targetDevice) { setMessage({ text: `没有可用于该连接的${profile.type === 'wifi' ? '无线' : '有线'}网卡`, type: 'err' }); return }
    if (!window.confirm(`在 ${targetDevice} 上安全启用“${profile.name}”？试用后 ${NETWORK_ROLLBACK_SECONDS} 秒内未确认会恢复当前连接。`)) return
    setBusy('profile'); setMessage(null)
    try {
      const value = await activateNetworkConnection(profile.uuid, targetDevice)
      setTransaction(value.transaction); setClock(Date.now())
      setMessage({ text: '正在试用已保存的连接，请在检查正常后确认。', type: 'ok' })
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }

  const removeProfile = async (profile: NetworkConnectionInfo) => {
    if (!window.confirm(`确定删除连接配置“${profile.name}”？此操作不可恢复。`)) return
    setBusy('profile'); setMessage(null)
    try {
      await deleteNetworkConnection(profile.uuid)
      setMessage({ text: '连接配置已删除', type: 'ok' }); await load(true)
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }

  const testPing = async () => {
    setBusy('ping'); setPingResult(null)
    try {
      const value = await pingNetworkTarget(pingTarget)
      setPingResult(value.reachable ? `${value.target} 可以连接` : `${value.target} 没有响应（部分网络可能禁止 Ping）`)
    } catch (e) { setPingResult(err(e)) }
    finally { setBusy(null) }
  }

  return (
    <div className="settings-section">
      <div className="settings-section-title"><h3>网络</h3><p>管理有线、Wi-Fi 和连接配置；所有网络切换均带限时确认与自动回滚。</p></div>
      {message && <div className={`device-settings-message ${message.type}`}>{message.text}</div>}
      {transaction && <TransactionCard transaction={transaction} remaining={remaining} busy={busy === 'transaction'} onConfirm={confirmTransaction} onRollback={rollbackTransaction} />}
      {!data ? <div className="device-settings-state">正在读取网络信息……</div> : <>
        {data.error && <div className="device-settings-message err">{data.error}</div>}
        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>网络概览</h4><p>管理方式：{data.manager}</p></div><button className="settings-ghost-btn" disabled={busy !== null} onClick={() => void load()}>刷新</button></div>
          <div className="network-interface-list">{data.interfaces.map(item => <InterfaceCard key={item.device} item={item} active={device === item.device} onClick={() => setDevice(item.device)} />)}</div>
        </section>

        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>设备名称</h4><p>局域网和系统日志中用于识别这台盒子。</p></div></div>
          <div className="settings-form-row"><label><span>Hostname</span><input value={hostname} maxLength={253} onChange={e => setHostname(e.target.value)} /></label><button className="settings-primary-btn" disabled={busy !== null || hostname === data.hostname} onClick={saveHostname}>{busy === 'hostname' ? '保存中……' : '保存设备名'}</button></div>
        </section>

        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>安全配置连接</h4><p>{selected ? `${selected.device} · ${selected.type === 'wifi' ? 'Wi-Fi' : '有线网络'} · ${selected.connection || '尚无活动连接'}` : '请选择网卡'}</p></div>{selected?.type === 'wifi' && <button className="settings-ghost-btn" disabled={busy !== null} onClick={scanWifi}>{busy === 'wifi' ? '扫描中……' : '扫描 Wi-Fi'}</button>}</div>
          {wifiNetworks.length > 0 && <div className="wifi-network-list">{wifiNetworks.map(network => <button key={network.ssid} className={ssid === network.ssid ? 'active' : ''} onClick={() => chooseWifi(network)}><span><b>{network.ssid}</b><small>{network.security}</small></span><strong>{network.signal}%</strong></button>)}</div>}
          <div className="network-config-grid">
            <label><span>配置网卡</span><select value={device} onChange={e => setDevice(e.target.value)}>{data.interfaces.map(item => <option key={item.device} value={item.device}>{item.device}（{stateText(item.state)}）</option>)}</select></label>
            <label><span>保存后的连接名称（可留空）</span><input placeholder={selected?.type === 'wifi' ? '自动使用 Wi-Fi 名称' : `LAN ${selected?.device || ''}`} value={profileName} onChange={e => setProfileName(e.target.value)} /></label>
            {selected?.type === 'wifi' && <>
              <label><span>Wi-Fi SSID</span><input value={ssid} maxLength={32} onChange={e => setSsid(e.target.value)} /></label>
              <label><span>安全方式</span><select value={wifiSecurity} onChange={e => setWifiSecurity(e.target.value as 'wpa-psk' | 'sae' | 'open')}><option value="wpa-psk">WPA/WPA2 密码</option><option value="sae">WPA3/SAE</option><option value="open">开放网络</option></select></label>
              {wifiSecurity !== 'open' && <label className="wide"><span>Wi-Fi 密码</span><input type="password" autoComplete="new-password" placeholder={selected.connection_uuid && selected.ssid === ssid ? '留空沿用当前密码，或输入新密码' : '8–63 个字符，不会显示或写入日志'} value={wifiPassword} onChange={e => setWifiPassword(e.target.value)} /></label>}
            </>}
            <label><span>IPv4 地址模式</span><select value={method} onChange={e => setMethod(e.target.value as 'auto' | 'manual')}><option value="auto">DHCP 自动获取</option><option value="manual">静态 IPv4</option></select></label>
            {method === 'manual' && <>
              <label><span>IP 地址/前缀</span><input placeholder="192.168.1.100/24" value={address} onChange={e => setAddress(e.target.value)} /></label>
              <label><span>默认网关（可留空）</span><input placeholder="192.168.1.1" value={gateway} onChange={e => setGateway(e.target.value)} /></label>
              <label className="wide"><span>DNS（逗号分隔，最多 4 个，可留空）</span><input placeholder="223.5.5.5, 119.29.29.29" value={dns} onChange={e => setDns(e.target.value)} /></label>
            </>}
          </div>
          {!selected?.configurable && <div className="device-settings-note warning">该网卡未由 NetworkManager 托管，不能从网页修改。</div>}
          <div className="device-settings-note warning">系统先创建临时连接。试用新配置后，需要使用新 IP 地址登录 Web 控制台并回到此页面确认；若 {NETWORK_ROLLBACK_SECONDS} 秒内未确认，系统会自动恢复原连接。</div>
          <div className="settings-actions"><button className="settings-primary-btn" disabled={busy !== null || !!transaction && ['scheduled', 'activating', 'awaiting_confirmation', 'committing', 'rolling_back'].includes(transaction.status) || !selected?.configurable || !data.config_supported} onClick={applyNetwork}>{busy === 'network' ? '正在准备安全切换……' : '试用新配置'}</button></div>
        </section>

        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>已保存的连接</h4><p>连接使用 UUID 管理；当前活动连接不能直接删除。</p></div></div>
          {!data.connections.length ? <div className="device-settings-state">没有已保存的有线或 Wi-Fi 连接。</div> : <div className="network-profile-list">{data.connections.map(profile => <div key={profile.uuid}><span><b>{profile.name}</b><small>{profile.type === 'wifi' ? `Wi-Fi${profile.ssid ? ` · ${profile.ssid}` : ''} · ${profile.security}` : '有线'} · {profile.ipv4_method === 'manual' ? profile.addresses.join(', ') || '静态地址' : 'DHCP'} · {profile.autoconnect ? '自动连接' : '不自动连接'}</small>{(profile.gateway || profile.dns.length > 0) && <small>网关 {profile.gateway || '未设置'} · DNS {profile.dns.join(', ') || '未设置'}</small>}<small>{profile.uuid}</small></span><em className={profile.active ? 'active' : ''}>{profile.active ? `${profile.device} 使用中` : '未启用'}</em><button className="settings-ghost-btn" disabled={busy !== null || profile.active || !data.config_supported} onClick={() => activateProfile(profile)}>安全启用</button><button className="settings-ghost-btn danger" disabled={busy !== null || profile.active} onClick={() => removeProfile(profile)}>删除</button></div>)}</div>}
        </section>

        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>连通性测试</h4><p>测试指定 IPv4 地址是否响应；对端禁止 Ping 时可能显示不通。</p></div></div>
          <div className="settings-form-row"><label><span>目标 IPv4</span><input placeholder="192.168.1.1" value={pingTarget} onChange={e => setPingTarget(e.target.value)} /></label><button className="settings-ghost-btn" disabled={busy !== null || !pingTarget.trim()} onClick={testPing}>{busy === 'ping' ? '测试中……' : '开始测试'}</button><div className="settings-inline-facts">{pingResult && <b>{pingResult}</b>}</div></div>
        </section>
      </>}
    </div>
  )
}

function TransactionCard({ transaction, remaining, busy, onConfirm, onRollback }: {
  transaction: NetworkTransaction
  remaining: number
  busy: boolean
  onConfirm: (baseUrl?: string) => void
  onRollback: () => void
}) {
  const waiting = transaction.status === 'awaiting_confirmation'
  const active = ['scheduled', 'activating', 'awaiting_confirmation', 'committing', 'rolling_back'].includes(transaction.status)
  const targetIp = transaction.target_addresses[0]?.split('/', 1)[0] || ''
  const currentIp = window.location.hostname
  const port = window.location.port ? `:${window.location.port}` : ''
  const targetBase = targetIp ? `${window.location.protocol}//${targetIp}${port}` : ''
  return <section className={`network-transaction-card ${transaction.status}`}>
    <div><span>网络安全切换</span><strong>{transactionText(transaction.status)}</strong></div>
    <p>网卡 {transaction.device} · {transaction.profile_name || '连接配置'}{active && ` · 剩余 ${remaining} 秒`}</p>
    {transaction.target_addresses.length > 0 && <small>新地址：{transaction.target_addresses.join('、')}</small>}
    {transaction.error && <small className="error">{transaction.error}</small>}
    {active && <div className="network-transaction-progress"><span style={{ width: `${Math.max(0, Math.min(100, remaining / NETWORK_ROLLBACK_SECONDS * 100))}%` }} /></div>}
    <div className="settings-actions">
      {waiting && <button className="settings-primary-btn" disabled={busy} onClick={() => onConfirm()}>在当前地址确认</button>}
      {active && targetBase && targetIp !== currentIp && <button className="settings-primary-btn" disabled={busy} onClick={() => onConfirm(targetBase)}>通过新 IP 检查并确认</button>}
      {active && <button className="settings-ghost-btn danger" disabled={busy} onClick={onRollback}>立即恢复原网络</button>}
    </div>
  </section>
}

function InterfaceCard({ item, active, onClick }: { item: NetworkInterfaceInfo; active: boolean; onClick: () => void }) {
  return <button className={`network-interface-card ${active ? 'active' : ''}`} onClick={onClick}>
    <div><b>{item.device}</b><span className={item.state === 'connected' ? 'connected' : ''}>{stateText(item.state)}</span></div>
    <p>{item.type === 'wifi' ? `Wi-Fi${item.ssid ? ` · ${item.ssid}` : ''}` : '有线网络'} · {item.connection || '无连接'}</p>
    <strong>{item.addresses[0] || '未分配 IPv4'}</strong>
    <small>MAC {item.mac || '-'}</small>
  </button>
}
