import { useEffect, useMemo, useRef, useState } from 'react'
import {
  fetchDailyRebootSettings,
  fetchSystemTimezones,
  saveDailyRebootSettings,
  saveSystemTimezone,
  type DailyRebootSettings,
} from '../../api/client'

const errorMessage = (error: unknown): string => {
  const detail = (error as { response?: { data?: { detail?: unknown } } })?.response?.data?.detail
  if (typeof detail === 'string' && detail) return detail
  return error instanceof Error ? error.message : String(error)
}

const HOURS = Array.from({ length: 24 }, (_, value) => String(value).padStart(2, '0'))
const MINUTES = Array.from({ length: 60 }, (_, value) => String(value).padStart(2, '0'))

const formatBoxTime = (epochMs: number, utcOffsetMinutes: number): string => {
  const value = new Date(epochMs + utcOffsetMinutes * 60_000)
  const pad = (part: number) => String(part).padStart(2, '0')
  return `${value.getUTCFullYear()}-${pad(value.getUTCMonth() + 1)}-${pad(value.getUTCDate())} ` +
    `${pad(value.getUTCHours())}:${pad(value.getUTCMinutes())}:${pad(value.getUTCSeconds())}`
}

interface ClockAnchor {
  boxEpochMs: number
  browserEpochMs: number
  utcOffsetMinutes: number
}

export default function SystemSettingsSection() {
  const [settings, setSettings] = useState<DailyRebootSettings | null>(null)
  const [enabled, setEnabled] = useState(false)
  const [rebootTime, setRebootTime] = useState('04:00')
  const [timezones, setTimezones] = useState<string[]>([])
  const [timezoneDraft, setTimezoneDraft] = useState('')
  const [boxTime, setBoxTime] = useState('正在同步……')
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState<'reboot' | 'timezone' | null>(null)
  const [message, setMessage] = useState<{ text: string; type: 'ok' | 'err' } | null>(null)
  const clockAnchor = useRef<ClockAnchor | null>(null)

  const syncClock = (value: DailyRebootSettings) => {
    const anchor = {
      boxEpochMs: value.current_time_epoch_ms,
      browserEpochMs: Date.now(),
      utcOffsetMinutes: value.utc_offset_minutes,
    }
    clockAnchor.current = anchor
    setBoxTime(formatBoxTime(anchor.boxEpochMs, anchor.utcOffsetMinutes))
  }

  const applySettings = (value: DailyRebootSettings, resetDraft = true) => {
    setSettings(value)
    syncClock(value)
    if (resetDraft) {
      setEnabled(value.enabled)
      setRebootTime(value.time)
      setTimezoneDraft(value.timezone)
    }
  }

  useEffect(() => {
    let active = true
    Promise.all([fetchDailyRebootSettings(), fetchSystemTimezones()])
      .then(([value, zones]) => {
        if (!active) return
        applySettings(value)
        setTimezones(zones)
      })
      .catch(error => { if (active) setMessage({ text: `读取系统设置失败：${errorMessage(error)}`, type: 'err' }) })
      .finally(() => { if (active) setLoading(false) })

    const clockTimer = window.setInterval(() => {
      const anchor = clockAnchor.current
      if (!anchor) return
      setBoxTime(formatBoxTime(
        anchor.boxEpochMs + Date.now() - anchor.browserEpochMs,
        anchor.utcOffsetMinutes,
      ))
    }, 1000)
    const resyncTimer = window.setInterval(() => {
      fetchDailyRebootSettings().then(value => {
        if (active) applySettings(value, false)
      }).catch(() => {})
    }, 60_000)
    return () => {
      active = false
      window.clearInterval(clockTimer)
      window.clearInterval(resyncTimer)
    }
  }, [])

  const rebootDirty = useMemo(() => (
    !!settings && (settings.enabled !== enabled || settings.time !== rebootTime)
  ), [settings, enabled, rebootTime])
  const timezoneDirty = !!settings && timezoneDraft !== settings.timezone
  const [rebootHour = '04', rebootMinute = '00'] = rebootTime.split(':')

  const saveReboot = async () => {
    setSaving('reboot'); setMessage(null)
    try {
      const value = await saveDailyRebootSettings(enabled, rebootTime)
      applySettings(value)
      setMessage({ text: enabled ? `已设置每天 ${rebootTime} 自动重启` : '已关闭每日定时重启', type: 'ok' })
    } catch (error) {
      setMessage({ text: errorMessage(error), type: 'err' })
    } finally { setSaving(null) }
  }

  const saveTimezone = async () => {
    if (!timezones.includes(timezoneDraft)) {
      setMessage({ text: '请选择有效的 IANA 时区', type: 'err' })
      return
    }
    if (!window.confirm(`确定把盒子时区修改为 ${timezoneDraft}？定时重启将按新时区计算。`)) return
    setSaving('timezone'); setMessage(null)
    try {
      const value = await saveSystemTimezone(timezoneDraft)
      applySettings(value)
      setMessage({ text: `设备时区已修改为 ${timezoneDraft}`, type: 'ok' })
    } catch (error) {
      setMessage({ text: errorMessage(error), type: 'err' })
    } finally { setSaving(null) }
  }

  if (loading) return <div className="device-settings-state">正在读取系统设置……</div>

  return (
    <div className="settings-section">
      <div className="settings-section-title"><h3>系统</h3><p>盒子时间、时区和计划重启。</p></div>
      {message && <div className={`device-settings-message ${message.type}`}>{message.text}</div>}

      <section className="device-settings-card">
        <div className="device-settings-card-head"><div><h4>日期与时间</h4><p>时间直接来自盒子系统，每分钟自动校准。</p></div></div>
        <div className="system-clock-panel">
          <div><span>盒子当前时间（24 小时制）</span><strong>{boxTime}</strong></div>
          <div><span>当前时区</span><b>{settings?.timezone || '未知'}</b></div>
        </div>
        <div className="settings-form-row timezone-row">
          <label><span>修改时区</span><select value={timezoneDraft} disabled={saving !== null} onChange={e => setTimezoneDraft(e.target.value)}>{timezones.map(zone => <option key={zone} value={zone}>{zone}</option>)}</select></label>
          <button className="settings-primary-btn" disabled={!timezoneDirty || saving !== null} onClick={saveTimezone}>
            {saving === 'timezone' ? '应用中……' : '应用时区'}
          </button>
        </div>
      </section>

      <section className="device-settings-card">
        <div className="device-settings-card-head">
          <div><h4>每日定时重启</h4><p>由 systemd timer 执行，无需保持网页打开。</p></div>
          <label className="system-switch"><input type="checkbox" checked={enabled} disabled={saving !== null} onChange={e => setEnabled(e.target.checked)} /><span /><b>{enabled ? '已开启' : '已关闭'}</b></label>
        </div>
        <div className="settings-form-row">
          <label><span>每天重启时间（24 小时制）</span>
            <div className="system-time-picker">
              <select value={rebootHour} disabled={saving !== null} onChange={e => setRebootTime(`${e.target.value}:${rebootMinute}`)}>{HOURS.map(v => <option key={v} value={v}>{v} 时</option>)}</select>
              <b>:</b>
              <select value={rebootMinute} disabled={saving !== null} onChange={e => setRebootTime(`${rebootHour}:${e.target.value}`)}>{MINUTES.map(v => <option key={v} value={v}>{v} 分</option>)}</select>
            </div>
          </label>
          <div className="settings-inline-facts">
            <span>定时器：<b className={settings?.active ? 'good' : ''}>{settings?.active ? '运行中' : '未运行'}</b></span>
            {settings?.enabled && <span>下次执行：<b>{settings.next_run || '等待系统计算'}</b></span>}
          </div>
        </div>
        <div className="device-settings-note warning">到点后盒子会直接重启。视觉程序及后台服务需分别开启“开机自启”，才能在重启后恢复。</div>
        <div className="settings-actions"><button className="settings-primary-btn" disabled={!rebootDirty || saving !== null} onClick={saveReboot}>{saving === 'reboot' ? '保存中……' : '保存重启设置'}</button></div>
      </section>
    </div>
  )
}
