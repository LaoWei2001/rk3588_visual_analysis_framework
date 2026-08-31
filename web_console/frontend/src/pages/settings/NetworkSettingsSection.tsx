import { useEffect, useMemo, useState } from 'react'
import {
  activateNetworkConnection,
  confirmNetworkTransaction,
  deleteNetworkConnection,
  fetchNetworkSettings,
  fetchNetworkTransaction,
  pingNetworkTarget,
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
const stateText = (value: string) => ({ connected: '已连接', disconnected: '未连接', unavailable: '暂不可用', unmanaged: '不能修改', connecting: '正在连接' }[value] || value)
const transactionText = (value: string) => ({
  scheduled: '等待切换', activating: '正在激活新网络', awaiting_confirmation: '等待确认',
  committing: '正在保存', rolling_back: '正在恢复原网络', confirmed: '已确认保存',
  rolled_back: '已恢复原网络', rollback_failed: '自动恢复失败',
}[value] || value)

type Busy = 'hostname' | 'network' | 'refresh' | 'wifi' | 'profile' | 'ping' | 'transaction' | null
const NETWORK_ROLLBACK_SECONDS = 60

const prefixToSubnetMask = (value: string | undefined) => {
  const prefix = Number(value)
  if (!Number.isInteger(prefix) || prefix < 0 || prefix > 32) return '255.255.255.0'
  const bits = `${'1'.repeat(prefix)}${'0'.repeat(32 - prefix)}`
  return [0, 8, 16, 24].map(offset => parseInt(bits.slice(offset, offset + 8), 2)).join('.')
}

const subnetMaskToPrefix = (value: string) => {
  const octets = value.trim().split('.')
  if (octets.length !== 4 || octets.some(item => !/^\d{1,3}$/.test(item) || Number(item) > 255)) return null
  const bits = octets.map(item => Number(item).toString(2).padStart(8, '0')).join('')
  if (!/^1*0*$/.test(bits)) return null
  const firstZero = bits.indexOf('0')
  return firstZero === -1 ? 32 : firstZero
}

export default function NetworkSettingsSection() {
  const [data, setData] = useState<NetworkSettings | null>(null)
  const [hostname, setHostname] = useState('')
  const [device, setDevice] = useState('')
  const [method, setMethod] = useState<'auto' | 'manual'>('auto')
  const [address, setAddress] = useState('')
  const [subnetMask, setSubnetMask] = useState('255.255.255.0')
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
    const [currentAddress = '', currentPrefix] = (selected.addresses[0] || '').split('/', 2)
    setAddress(currentAddress)
    setSubnetMask(prefixToSubnetMask(currentPrefix))
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
      if (!value.networks.length) setMessage({ text: '没有找到附近的无线网络', type: 'err' })
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }

  const chooseWifi = (network: WifiNetworkInfo) => {
    setSsid(network.ssid)
    if (network.ssid !== selected?.ssid) setProfileName(`无线 ${network.ssid}`)
    const security = network.security.toUpperCase()
    setWifiSecurity(security === 'OPEN' || security === '--' ? 'open' : security.includes('SAE') || security.includes('WPA3') ? 'sae' : 'wpa-psk')
    setWifiPassword('')
  }

  const applyNetwork = async () => {
    if (!selected) return
    if (selected.type === 'wifi' && !ssid.trim()) {
      setMessage({ text: '请先选择或填写无线网络名称', type: 'err' }); return
    }
    let requestedAddress = ''
    if (method === 'manual') {
      const prefix = subnetMaskToPrefix(subnetMask)
      if (prefix === null) {
        setMessage({ text: '子网掩码格式不正确，例如 255.255.255.0', type: 'err' }); return
      }
      requestedAddress = `${address.trim()}/${prefix}`
    }
    const target = method === 'manual' ? address || '未填写' : '自动获取地址'
    const warning = `即将在 ${selected.device} 上应用网络设置（${target}）。\n如果 IP 地址改变，请使用新地址重新打开本页面并确认；${NETWORK_ROLLBACK_SECONDS} 秒内没有确认时，系统会自动使用修改前的连接。确定继续？`
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
        address: requestedAddress,
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
      setMessage({ text: '正在应用网络设置。连接正常后，请在倒计时结束前确认。', type: 'ok' })
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
        ? '已确认使用该连接。'
        : transaction.old_uuid
          ? '新网络已经确认并保存，修改前的连接仍保留但不会自动使用。'
          : '新网络已经确认并保存。'
      setMessage({ text: savedText, type: 'ok' })
      await load(true)
    } catch (e) { setMessage({ text: `确认失败：${err(e)}`, type: 'err' }) }
    finally { setBusy(null) }
  }

  const activateProfile = async (profile: NetworkConnectionInfo) => {
    const targetDevice = selected?.type === profile.type
      ? selected.device
      : data?.interfaces.find(item => item.type === profile.type)?.device
    if (!targetDevice) { setMessage({ text: `没有可用于该连接的${profile.type === 'wifi' ? '无线' : '有线'}网卡`, type: 'err' }); return }
    if (!window.confirm(`确定在 ${targetDevice} 上使用“${profile.name}”？如果 ${NETWORK_ROLLBACK_SECONDS} 秒内没有确认，系统会自动使用修改前的连接。`)) return
    setBusy('profile'); setMessage(null)
    try {
      const value = await activateNetworkConnection(profile.uuid, targetDevice)
      setTransaction(value.transaction); setClock(Date.now())
      setMessage({ text: '正在使用所选连接，请在检查正常后确认。', type: 'ok' })
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }

  const removeProfile = async (profile: NetworkConnectionInfo) => {
    if (!window.confirm(`确定删除连接“${profile.name}”？删除后需要重新填写才能再次使用。`)) return
    setBusy('profile'); setMessage(null)
    try {
      await deleteNetworkConnection(profile.uuid)
      setMessage({ text: '连接已删除', type: 'ok' }); await load(true)
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
      <div className="settings-section-title"><h3>网络</h3><p>查看当前网络，也可以在后续维护时调整有线或无线连接。</p></div>
      {message && <div className={`device-settings-message ${message.type}`}>{message.text}</div>}
      {transaction && <TransactionCard transaction={transaction} remaining={remaining} busy={busy === 'transaction'} onConfirm={confirmTransaction} />}
      {!data ? <div className="device-settings-state">正在读取网络信息……</div> : <>
        {data.error && <div className="device-settings-message err">{data.error}</div>}
        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>当前网络</h4><p>选择网卡可以查看或调整它的设置。</p></div><button className="settings-ghost-btn" disabled={busy !== null} onClick={() => void load()}>刷新</button></div>
          <div className="network-interface-list">{data.interfaces.map(item => <InterfaceCard key={item.device} item={item} active={device === item.device} onClick={() => setDevice(item.device)} />)}</div>
        </section>

        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>设备名称</h4><p>用于在局域网和系统记录中识别这台设备。</p></div></div>
          <div className="settings-form-row"><label><span>名称</span><input value={hostname} maxLength={253} onChange={e => setHostname(e.target.value)} /></label><button className="settings-primary-btn" disabled={busy !== null || hostname === data.hostname} onClick={saveHostname}>{busy === 'hostname' ? '保存中……' : '保存名称'}</button></div>
        </section>

        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>调整连接</h4><p>{selected ? `${selected.device} · ${selected.type === 'wifi' ? '无线网络' : '有线网络'} · ${selected.connection || '当前没有连接'}` : '请选择网卡'}</p></div>{selected?.type === 'wifi' && <button className="settings-ghost-btn" disabled={busy !== null} onClick={scanWifi}>{busy === 'wifi' ? '扫描中……' : '搜索无线网络'}</button>}</div>
          {wifiNetworks.length > 0 && <div className="wifi-network-list">{wifiNetworks.map(network => <button key={network.ssid} className={ssid === network.ssid ? 'active' : ''} onClick={() => chooseWifi(network)}><span><b>{network.ssid}</b><small>{network.security}</small></span><strong>{network.signal}%</strong></button>)}</div>}
          <div className="network-config-grid">
            <label><span>配置网卡</span><select value={device} onChange={e => setDevice(e.target.value)}>{data.interfaces.map(item => <option key={item.device} value={item.device}>{item.device}（{stateText(item.state)}）</option>)}</select></label>
            <label><span>连接名称（可留空）</span><input placeholder={selected?.type === 'wifi' ? '默认使用无线网络名称' : `有线 ${selected?.device || ''}`} value={profileName} onChange={e => setProfileName(e.target.value)} /></label>
            {selected?.type === 'wifi' && <>
              <label><span>无线网络名称</span><input value={ssid} maxLength={32} onChange={e => setSsid(e.target.value)} /></label>
              <label><span>密码方式</span><select value={wifiSecurity} onChange={e => setWifiSecurity(e.target.value as 'wpa-psk' | 'sae' | 'open')}><option value="wpa-psk">WPA/WPA2</option><option value="sae">WPA3</option><option value="open">无密码</option></select></label>
              {wifiSecurity !== 'open' && <label className="wide"><span>Wi-Fi 密码</span><input type="password" autoComplete="new-password" placeholder={selected.connection_uuid && selected.ssid === ssid ? '留空沿用当前密码，或输入新密码' : '8–63 个字符，不会显示或写入日志'} value={wifiPassword} onChange={e => setWifiPassword(e.target.value)} /></label>}
            </>}
            <label><span>IP 地址</span><select value={method} onChange={e => setMethod(e.target.value as 'auto' | 'manual')}><option value="auto">自动获取</option><option value="manual">使用固定地址</option></select></label>
            {method === 'manual' && <>
              <label><span>固定 IP 地址</span><input placeholder="192.168.1.100" value={address} onChange={e => setAddress(e.target.value)} /></label>
              <label><span>子网掩码</span><input placeholder="255.255.255.0" value={subnetMask} onChange={e => setSubnetMask(e.target.value)} /></label>
              <label><span>默认网关（可留空）</span><input placeholder="192.168.1.1" value={gateway} onChange={e => setGateway(e.target.value)} /></label>
              <label className="wide"><span>DNS（逗号分隔，最多 4 个，可留空）</span><input placeholder="223.5.5.5, 119.29.29.29" value={dns} onChange={e => setDns(e.target.value)} /></label>
            </>}
          </div>
          {!selected?.configurable && <div className="device-settings-note warning">系统没有接管这张网卡，当前不能从网页修改。</div>}
          <div className="device-settings-note warning">应用后如果 IP 地址改变，请使用新地址重新打开本页面并确认。{NETWORK_ROLLBACK_SECONDS} 秒内没有确认时，系统会自动使用修改前的连接。</div>
          <div className="settings-actions"><button className="settings-primary-btn" disabled={busy !== null || !!transaction && ['scheduled', 'activating', 'awaiting_confirmation', 'committing', 'rolling_back'].includes(transaction.status) || !selected?.configurable || !data.config_supported} onClick={applyNetwork}>{busy === 'network' ? '正在应用……' : '应用设置'}</button></div>
        </section>

        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>已保存的连接</h4><p>正在使用的连接不能直接删除。</p></div></div>
          {!data.connections.length ? <div className="device-settings-state">没有已保存的有线或无线连接。</div> : <div className="network-profile-list">{data.connections.map(profile => <div key={profile.uuid}><span><b>{profile.name}</b><small>{profile.type === 'wifi' ? `无线网络${profile.ssid ? ` · ${profile.ssid}` : ''}` : '有线网络'} · {profile.ipv4_method === 'manual' ? profile.addresses.join(', ') || '固定地址' : '自动获取地址'} · {profile.autoconnect ? '开机自动使用' : '开机不自动使用'}</small>{(profile.gateway || profile.dns.length > 0) && <small>网关 {profile.gateway || '未设置'} · DNS {profile.dns.join(', ') || '未设置'}</small>}</span><em className={profile.active ? 'active' : ''}>{profile.active ? `${profile.device} 正在使用` : '未使用'}</em><button className="settings-ghost-btn" disabled={busy !== null || profile.active || !data.config_supported} onClick={() => activateProfile(profile)}>使用</button><button className="settings-ghost-btn danger" disabled={busy !== null || profile.active} onClick={() => removeProfile(profile)}>删除</button></div>)}</div>}
        </section>

        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>连通性测试</h4><p>测试指定 IPv4 地址是否响应；对端禁止 Ping 时可能显示不通。</p></div></div>
          <div className="settings-form-row"><label><span>目标 IPv4</span><input placeholder="192.168.1.1" value={pingTarget} onChange={e => setPingTarget(e.target.value)} /></label><button className="settings-ghost-btn" disabled={busy !== null || !pingTarget.trim()} onClick={testPing}>{busy === 'ping' ? '测试中……' : '开始测试'}</button><div className="settings-inline-facts">{pingResult && <b>{pingResult}</b>}</div></div>
        </section>
      </>}
    </div>
  )
}

function TransactionCard({ transaction, remaining, busy, onConfirm }: {
  transaction: NetworkTransaction
  remaining: number
  busy: boolean
  onConfirm: (baseUrl?: string) => void
}) {
  const waiting = transaction.status === 'awaiting_confirmation'
  const active = ['scheduled', 'activating', 'awaiting_confirmation', 'committing', 'rolling_back'].includes(transaction.status)
  const targetIp = transaction.target_addresses[0]?.split('/', 1)[0] || ''
  const currentIp = window.location.hostname
  const port = window.location.port ? `:${window.location.port}` : ''
  const targetBase = targetIp ? `${window.location.protocol}//${targetIp}${port}` : ''
  return <section className={`network-transaction-card ${transaction.status}`}>
    <div><span>正在应用网络设置</span><strong>{transactionText(transaction.status)}</strong></div>
    <p>网卡 {transaction.device} · {transaction.profile_name || '连接配置'}{active && ` · 剩余 ${remaining} 秒`}</p>
    {transaction.target_addresses.length > 0 && <small>新地址：{transaction.target_addresses.join('、')}</small>}
    {transaction.error && <small className="error">{transaction.error}</small>}
    {active && <div className="network-transaction-progress"><span style={{ width: `${Math.max(0, Math.min(100, remaining / NETWORK_ROLLBACK_SECONDS * 100))}%` }} /></div>}
    <div className="settings-actions">
      {waiting && <button className="settings-primary-btn" disabled={busy} onClick={() => onConfirm()}>在当前地址确认</button>}
      {active && targetBase && targetIp !== currentIp && <button className="settings-primary-btn" disabled={busy} onClick={() => onConfirm(targetBase)}>使用新地址确认</button>}
    </div>
  </section>
}

function InterfaceCard({ item, active, onClick }: { item: NetworkInterfaceInfo; active: boolean; onClick: () => void }) {
  return <button className={`network-interface-card ${active ? 'active' : ''}`} onClick={onClick}>
    <div><b>{item.device}</b><span className={item.state === 'connected' ? 'connected' : ''}>{stateText(item.state)}</span></div>
    <p>{item.type === 'wifi' ? `无线网络${item.ssid ? ` · ${item.ssid}` : ''}` : '有线网络'} · {item.connection || '无连接'}</p>
    <strong>{item.addresses[0] || '未分配 IPv4'}</strong>
    <small>MAC {item.mac || '-'}</small>
  </button>
}
