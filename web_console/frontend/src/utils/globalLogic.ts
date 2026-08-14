export interface GlobalLogicEntry {
  enable: boolean
  logic: string
  channels: number[]
  channels_explicit?: boolean
  poll_interval_ms: number
  logic_parameters: Record<string, unknown>
  report_policy?: Record<string, unknown>
  report_parameters?: Record<string, unknown>
  media_source_channel_id?: number
}
