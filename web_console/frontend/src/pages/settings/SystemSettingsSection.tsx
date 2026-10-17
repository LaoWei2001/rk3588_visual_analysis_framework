import { useEffect, useRef, useState } from 'react'
import { useAutoSync, useSyncedDraft } from '../../hooks/useAutoSync'
import ConfigSyncNotice from '../../components/ConfigSyncNotice'
import {
  fetchDailyRebootSettings,
  fetchSystemTimezones,
  saveDailyRebootSettings,
  saveSystemTimezone,
  saveSystemTime,
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
  const [rebootDraft, setRebootDraft, rebootSync] = useSyncedDraft(settings ? { enabled: settings.enabled, time: settings.time } : null, { enabled: false, time: '04:00' })
  const { enabled, time: rebootTime } = rebootDraft
  const setEnabled = (value: boolean) => setRebootDraft(current => ({ ...current, enabled: value }))
  const setRebootTime = (value: string) => setRebootDraft(current => ({ ...current, time: value }))
  const [timezones, setTimezones] = useState<string[]>([])
  const [timezoneDraft, setTimezoneDraft, timezoneSync] = useSyncedDraft(settings?.timezone ?? null, '')
  const [boxTime, setBoxTime] = useState('正在同步……')
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState<'reboot' | 'timezone' | 'clock' | null>(null)
  const [timeDraft, setTimeDraft] = useState<{ value: string; timezone: string; ntp: boolean | null | undefined } | null>(null)
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

  const applySettings = (value: DailyRebootSettings) => {
    setSettings(value)
    setMessage(current => current?.text.startsWith('读取系统设置失败：') ? null : current)
    syncClock(value)
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
    return () => {
      active = false
      window.clearInterval(clockTimer)
    }
  }, [])

  const syncError = useAutoSync(() => Promise.all([fetchDailyRebootSettings(), timezones.length ? Promise.resolve(timezones) : fetchSystemTimezones()]),
    ([value, zones]) => { applySettings(value); setTimezones(zones) }, { enabled: !loading && saving === null, immediate: false })
  const rebootDirty = rebootSync.dirty
  const rebootNeedsSave = !!settings && (settings.unit_enabled !== enabled || settings.configured_enabled != null && settings.configured_enabled !== enabled)
  const timezoneDirty = timezoneSync.dirty
  const timeConflict = !!timeDraft && !!settings && (timeDraft.timezone !== settings.timezone || timeDraft.ntp !== settings.ntp_enabled)
  const timeUnavailable = !!syncError || !!settings?.time_error || settings?.ntp_enabled == null
  const [rebootHour = '04', rebootMinute = '00'] = rebootTime.split(':')

  const saveReboot = async () => {
    setSaving('reboot'); setMessage(null)
    try {
      const latest = await fetchDailyRebootSettings()
      applySettings(latest)
      if (latest.error || !rebootSync.isCurrent({ enabled: latest.enabled, time: latest.time })) throw new Error(latest.error || '底层重启设置已变化，请重新同步后再保存')
      const value = await saveDailyRebootSettings(enabled, rebootTime)
      applySettings(value)
      rebootSync.accept({ enabled: value.enabled, time: value.time })
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
      const latest = await fetchDailyRebootSettings()
      applySettings(latest)
      if (!timezoneSync.isCurrent(latest.timezone)) throw new Error('底层时区已变化，请重新同步后再保存')
      const value = await saveSystemTimezone(timezoneDraft)
      applySettings(value)
      timezoneSync.accept(value.timezone)
      setMessage({ text: `设备时区已修改为 ${timezoneDraft}`, type: 'ok' })
    } catch (error) {
      setMessage({ text: errorMessage(error), type: 'err' })
    } finally { setSaving(null) }
  }

  const saveClock = async (computer = false) => {
    if (!settings || (!computer && !timeDraft)) return
    const source = timeDraft && !computer ? { timezone: timeDraft.timezone, ntp: timeDraft.ntp }
      : { timezone: settings.timezone, ntp: settings.ntp_enabled }
    const localTime = timeDraft?.value
    if (!computer && !/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(?::\d{2})?$/.test(localTime || '')) {
      setMessage({ text: '请填写完整的日期、小时、分钟和秒', type: 'err' })
      return
    }
    setSaving('clock'); setMessage(null)
    try {
      const latest = await fetchDailyRebootSettings()
      applySettings(latest)
      if (latest.time_error || latest.ntp_enabled == null) throw new Error(latest.time_error || '无法确认自动校时状态，请稍后重试')
      if (source.timezone !== latest.timezone || source.ntp !== latest.ntp_enabled) throw new Error('底层时区或自动校时设置已变化，请重新同步后再设置时间')
      const value = await saveSystemTime({ timezone: latest.timezone, disable_ntp: latest.ntp_enabled,
        ...(computer ? { epoch_ms: Date.now() } : { local_time: localTime!.length === 16 ? localTime + ':00' : localTime }) })
      applySettings(value)
      setTimeDraft(null)
      setMessage({ text: computer ? '系统时间已与当前电脑同步' : '系统时间已修改', type: 'ok' })
    } catch (error) {
      setMessage({ text: errorMessage(error), type: 'err' })
    } finally { setSaving(null) }
  }

  if (loading) return <div className="device-settings-state">正在读取系统设置……</div>

  return (
    <div className="settings-section">
      <div className="settings-section-title"><h3>系统</h3></div>
      {message && <div className={`device-settings-message ${message.type}`}>{message.text}</div>}
      <ConfigSyncNotice error={syncError || settings?.error || ''} conflict={rebootSync.conflict || timezoneSync.conflict || timeConflict} onReload={() => { rebootSync.reset(); timezoneSync.reset(); setTimeDraft(null) }} />

      <section className="device-settings-card">
        <div className="device-settings-card-head"><div><h4>日期与时间</h4></div></div>
        <div className="system-clock-panel">
          <div><span>盒子当前时间（24 小时制）</span><strong>{boxTime}</strong></div>
          <div><span>当前时区</span><b>{settings?.timezone || '未知'}</b></div>
        </div>
        <p className="system-time-help">网络自动校时：{settings?.ntp_enabled == null ? '状态未知' : settings.ntp_enabled ? '已开启；手动应用时间或与电脑同步时会关闭自动校时。' : '未开启'}{settings?.can_ntp === false && '（系统未提供可用的网络校时服务）'}</p>
        {settings?.time_error && <div className="device-settings-message err">时间设置状态读取失败：{settings.time_error}</div>}
        <div className="settings-form-row system-datetime-row">
          <label><span>修改日期与时间（{settings?.timezone || '设备时区'}）</span>
            <input type="datetime-local" aria-label="修改日期与时间" step="1" min="1970-01-01T00:00:00"
              value={timeDraft?.value ?? (/^\d{4}-/.test(boxTime) ? boxTime.replace(' ', 'T') : '')}
              disabled={saving !== null || timeUnavailable}
              onChange={e => setTimeDraft(current => ({ value: e.target.value, timezone: current?.timezone ?? settings?.timezone ?? '', ntp: current ? current.ntp : settings?.ntp_enabled }))} />
          </label>
          <button className="settings-primary-btn" disabled={!timeDraft?.value || timeConflict || timeUnavailable || saving !== null} onClick={() => void saveClock()}>
            {saving === 'clock' ? '应用中……' : settings?.ntp_enabled ? '关闭自动校时并应用时间' : '应用时间'}
          </button>
          <button className="settings-ghost-btn" disabled={timeUnavailable || saving !== null || timeConflict} onClick={() => void saveClock(true)}>与当前电脑同步</button>
          {timeDraft && <button className="settings-ghost-btn" disabled={saving !== null} onClick={() => setTimeDraft(null)}>取消时间修改</button>}
        </div>
        <p className="system-time-help">修改时间会影响日志和定时重启。</p>
        <div className="settings-form-row timezone-row">
          <label><span>修改时区</span><select value={timezoneDraft} disabled={saving !== null} onChange={e => setTimezoneDraft(e.target.value)}>{timezones.map(zone => <option key={zone} value={zone}>{zone}</option>)}</select></label>
          <button className="settings-primary-btn" disabled={!timezoneDirty || timezoneSync.conflict || !!syncError || saving !== null} onClick={saveTimezone}>
            {saving === 'timezone' ? '应用中……' : '应用时区'}
          </button>
        </div>
      </section>

      <section className="device-settings-card">
        <div className="device-settings-card-head">
          <div><h4>每日定时重启</h4></div>
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
        <div className="device-settings-note warning">到点后请求重启，共享冷却期内跳过。程序和后台服务需开启“开机自启”才能恢复。</div>
        {rebootNeedsSave && !rebootDirty && <div className="device-settings-note">当前定时器运行状态与开机设置不同。保存可让下次开机也使用当前选择。</div>}
        <div className="settings-actions"><button className="settings-primary-btn" disabled={!(rebootDirty || rebootNeedsSave) || rebootSync.conflict || !!syncError || !!settings?.error || saving !== null} onClick={saveReboot}>{saving === 'reboot' ? '保存中……' : '保存重启设置'}</button></div>
      </section>
    </div>
  )
}
