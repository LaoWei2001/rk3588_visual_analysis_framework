import { api, sharedRead } from './http'

// ── 逻辑参数清单(由 app 根目录 logics.json 提供, 后端 /apps/{name}/logics 透传) ──
export interface LogicParam {
  key: string
  type: 'int' | 'float' | 'string' | 'bool' | 'enum' | 'text' | 'json'
  json_type?: 'array' | 'object'                         // type=json 时约束容器类型
  label?: string
  default?: unknown
  min?: number
  max?: number
  step?: number
  unit?: string
  hot_reload?: 'preserve_state' | 'reset_state' | 'restart_required'
  options?: string[]
  placeholder?: string
  help?: string
}
export interface ReportField {
  key: string
  type: 'string' | 'number' | 'boolean' | 'json'
  label?: string
  help?: string
}
export interface LogicOutputDef {
  key: string
  type: 'string' | 'number' | 'integer' | 'boolean' | 'json'
  label?: string
  help?: string
}
export interface EventTypeDef {
  id: string
  label?: string
  help?: string
}
export interface BusinessField {
  path: string
  type: 'string' | 'number' | 'boolean' | 'json'
  label?: string
  help?: string
  required?: boolean
  default_selected?: boolean
}
export interface LogicDef {
  name: string
  label?: string
  params?: LogicParam[]
  parameters?: Record<string, unknown>                    // 模块 JSON Schema（C++/Web 同源）
  event_types?: EventTypeDef[]                            // 本逻辑可产生的 EventRequest.event_type
  report_template_ids?: string[]                          // 本 Logic 文件夹明确声明的模板 ID
  report_fields?: ReportField[]                            // C++ EventRequest.fields 只读字段清单
  business_fields?: BusinessField[]                        // 完整业务 JSON 字段目录
  outputs?: LogicOutputDef[]                               // 通道 logic 向全局 logic 公开的同帧变量
}

export interface AppLogics {
  channel_logics: LogicDef[]
  global_logics:  LogicDef[]
  model_types:    string[]
  error?: string
}

export const fetchAppLogics = (name: string) =>
  sharedRead(`logics:${name}`, () => api.get<AppLogics>(`/apps/${name}/logics`).then(r => r.data))

export interface LogicActionDef {
  id: string
  label?: string
  style?: 'default' | 'primary' | 'danger'
  confirm?: string
  help?: string
  payload?: Record<string, unknown>
}

export interface ChannelControlInfo {
  channel_id: number
  enabled: boolean
  logic: string
  logic_label: string
  actions: LogicActionDef[]
}

export interface GlobalLogicControlInfo {
  instance_id: string
  enabled: boolean
  logic: string
  logic_label: string
  actions: LogicActionDef[]
}

export interface LogicControlsResponse {
  socket_ready: boolean
  channels: ChannelControlInfo[]
  globals: GlobalLogicControlInfo[]
}

export const fetchLogicControls = (name: string) =>
  api.get<LogicControlsResponse>(`/apps/${name}/logic-actions`).then(r => r.data)

export const sendChannelAction = (
  name: string,
  channelId: number,
  action: string,
  payload: Record<string, unknown> = {},
) => api.post(`/apps/${name}/channels/${channelId}/actions/${encodeURIComponent(action)}`, { payload }).then(r => r.data)

export const sendGlobalLogicAction = (
  name: string,
  instanceId: string,
  action: string,
  payload: Record<string, unknown> = {},
) => api.post(
  `/apps/${name}/global-logics/${encodeURIComponent(instanceId)}/actions/${encodeURIComponent(action)}`,
  { payload },
).then(r => r.data)
