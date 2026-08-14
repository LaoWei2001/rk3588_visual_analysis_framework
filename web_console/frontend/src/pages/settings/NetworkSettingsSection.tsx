import { useEffect, useMemo, useState } from 'react'
import {
  fetchNetworkSettings,
  saveDeviceHostname,
  saveNetworkIPv4,
  type NetworkInterfaceInfo,
  type NetworkSettings,
} from '../../api/client'

const err = (e: unknown) => (e as { response?: { data?: { detail?: string } } })?.response?.data?.detail || (e instanceof Error ? e.message : String(e))
const stateText = (value: string) => ({ connected: '已连接', disconnected: '未连接', unavailable: '不可用', unmanaged: '未托管', connecting: '连接中' }[value] || value)

export default function NetworkSettingsSection() {
  const [data, setData] = useState<NetworkSettings | null>(null)
  const [hostname, setHostname] = useState('')
  const [device, setDevice] = useState('')
  const [method, setMethod] = useState<'auto' | 'manual'>('auto')
  const [address, setAddress] = useState('')
  const [gateway, setGateway] = useState('')
  const [dns, setDns] = useState('')
  const [busy, setBusy] = useState<'hostname' | 'network' | 'refresh' | null>(null)
  const [message, setMessage] = useState<{ text: string; type: 'ok' | 'err' } | null>(null)

  const load = async () => {
    setBusy('refresh')
    try {
      const value = await fetchNetworkSettings(); setData(value); setHostname(value.hostname)
      setDevice(current => value.interfaces.some(item => item.device === current)
        ? current : value.interfaces.find(item => item.state === 'connected')?.device || value.interfaces[0]?.device || '')
    } catch (e) { setMessage({ text: `读取网络信息失败：${err(e)}`, type: 'err' }) }
    finally { setBusy(null) }
  }
  useEffect(() => { load() }, [])
  const selected = useMemo(() => data?.interfaces.find(item => item.device === device) || null, [data, device])
  useEffect(() => {
    if (!selected) return
    setMethod(selected.ipv4_method === 'manual' ? 'manual' : 'auto')
    setAddress(selected.addresses[0] || '')
    setGateway(selected.gateway || '')
    setDns(selected.dns.join(', '))
  }, [selected?.device, selected?.connection_uuid])

  const saveHostname = async () => {
    setBusy('hostname'); setMessage(null)
    try { const value = await saveDeviceHostname(hostname); setHostname(value.hostname); setData(v => v ? { ...v, hostname: value.hostname } : v); setMessage({ text: '设备名已更新', type: 'ok' }) }
    catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }
  const applyNetwork = async () => {
    if (!selected?.connection_uuid) return
    const warning = `确定修改 ${selected.device} 的 IPv4 配置并重新激活连接？\n当前 Web 连接可能中断，请记住新的 IP 地址。`
    if (!window.confirm(warning)) return
    setBusy('network'); setMessage(null)
    try {
      await saveNetworkIPv4({ connection_uuid: selected.connection_uuid, method, address, gateway, dns: dns.split(',').map(v => v.trim()).filter(Boolean) })
      setMessage({ text: '网络配置已保存，连接正在重新激活；如果页面断开，请使用新 IP 重新访问。', type: 'ok' })
    } catch (e) { setMessage({ text: err(e), type: 'err' }) }
    finally { setBusy(null) }
  }

  return (
    <div className="settings-section">
      <div className="settings-section-title"><h3>网络</h3><p>查看盒子网卡状态，并管理设备名和 IPv4 地址。</p></div>
      {message && <div className={`device-settings-message ${message.type}`}>{message.text}</div>}
      {!data ? <div className="device-settings-state">正在读取网络信息……</div> : <>
        {data.error && <div className="device-settings-message err">{data.error}</div>}
        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>网络概览</h4><p>管理方式：{data.manager}</p></div><button className="settings-ghost-btn" disabled={busy !== null} onClick={load}>刷新</button></div>
          <div className="network-interface-list">{data.interfaces.map(item => <InterfaceCard key={item.device} item={item} active={device === item.device} onClick={() => setDevice(item.device)} />)}</div>
        </section>

        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>设备名称</h4><p>局域网和系统日志中用于识别这台盒子。</p></div></div>
          <div className="settings-form-row"><label><span>Hostname</span><input value={hostname} maxLength={253} onChange={e => setHostname(e.target.value)} /></label><button className="settings-primary-btn" disabled={busy !== null || hostname === data.hostname} onClick={saveHostname}>{busy === 'hostname' ? '保存中……' : '保存设备名'}</button></div>
        </section>

        <section className="device-settings-card">
          <div className="device-settings-card-head"><div><h4>IPv4 配置</h4><p>{selected ? `${selected.device} · ${selected.connection || '无活动连接'}` : '请选择网卡'}</p></div></div>
          <div className="network-config-grid">
            <label><span>配置网卡</span><select value={device} onChange={e => setDevice(e.target.value)}>{data.interfaces.map(item => <option key={item.device} value={item.device}>{item.device}（{stateText(item.state)}）</option>)}</select></label>
            <label><span>地址模式</span><select value={method} disabled={!selected?.configurable} onChange={e => setMethod(e.target.value as 'auto' | 'manual')}><option value="auto">DHCP 自动获取</option><option value="manual">静态 IPv4</option></select></label>
            {method === 'manual' && <><label><span>IP 地址/前缀</span><input placeholder="192.168.1.100/24" value={address} onChange={e => setAddress(e.target.value)} /></label><label><span>默认网关</span><input placeholder="192.168.1.1" value={gateway} onChange={e => setGateway(e.target.value)} /></label><label className="wide"><span>DNS（逗号分隔，最多 4 个）</span><input placeholder="223.5.5.5, 119.29.29.29" value={dns} onChange={e => setDns(e.target.value)} /></label></>}
          </div>
          {!selected?.configurable && <div className="device-settings-note warning">该网卡没有 NetworkManager 连接配置，只能查看，暂不能从网页修改。</div>}
          <div className="device-settings-note warning">应用网络配置可能立即改变盒子 IP 并中断当前页面。静态地址请先确认与网关处于同一网段且未被占用。</div>
          <div className="settings-actions"><button className="settings-primary-btn" disabled={busy !== null || !selected?.configurable || !data.config_supported} onClick={applyNetwork}>{busy === 'network' ? '应用中……' : '保存并应用 IPv4'}</button></div>
        </section>
      </>}
    </div>
  )
}

function InterfaceCard({ item, active, onClick }: { item: NetworkInterfaceInfo; active: boolean; onClick: () => void }) {
  return <button className={`network-interface-card ${active ? 'active' : ''}`} onClick={onClick}>
    <div><b>{item.device}</b><span className={item.state === 'connected' ? 'connected' : ''}>{stateText(item.state)}</span></div>
    <p>{item.type === 'wifi' ? 'Wi-Fi' : '有线网络'} · {item.connection || '无连接'}</p>
    <strong>{item.addresses[0] || '未分配 IPv4'}</strong>
    <small>MAC {item.mac || '-'}</small>
  </button>
}
