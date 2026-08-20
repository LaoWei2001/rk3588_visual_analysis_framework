export interface GlobalLogicEntry {
  instance_id: string
  enable: boolean
  logic: string
  channels: number[]
  poll_interval_ms: number
  logic_parameters: Record<string, unknown>
  report_policy?: Record<string, unknown>
  report_parameters?: Record<string, unknown>
  media_source_channel_id?: number
}
