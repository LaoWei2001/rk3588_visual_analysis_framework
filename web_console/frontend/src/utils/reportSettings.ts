import type { Edge, Node } from '@xyflow/react'

export const SHARED_POLICY_FIELDS = ['image_overlay', 'image_selection', 'video_overlay',
  'video_pre_sec', 'video_post_sec', 'video_fps', 'merge_window_sec'] as const

export function sharedReportSettings(data: Record<string, unknown>): Record<string, unknown> {
  const policy = (data.report_policy ?? {}) as Record<string, unknown>
  return {
    report_policy: Object.fromEntries(SHARED_POLICY_FIELDS.map(key => [key, policy[key]])),
    media_source_channel_id: data.media_source_channel_id,
  }
}

// A native policy belongs to its logic owner, not to an individual delivery.
// Keep delivery enabled/contract/connection independent while editing shared values.
export function updateNodeConfiguration(nodes: Node[], edges: Edge[], id: string,
  patch: Record<string, unknown>): Node[] {
  const current = nodes.find(node => node.id === id)
  const incoming = current?.type === 'report'
    ? edges.find(edge => edge.target === id && edge.targetHandle === 'report-in') : undefined
  const siblings = new Set(incoming ? edges.filter(edge => edge.source === incoming.source
    && edge.targetHandle === 'report-in').map(edge => edge.target) : [])
  const policy = patch.report_policy as Record<string, unknown> | undefined
  const sharedPolicy = policy ? Object.fromEntries(SHARED_POLICY_FIELDS
    .filter(key => key in policy).map(key => [key, policy[key]])) : {}
  return nodes.map(node => {
    if (node.id === id) return { ...node, data: { ...node.data, ...patch } }
    if (siblings.has(node.id)) return { ...node, data: {
      ...node.data,
      report_policy: { ...(node.data.report_policy as Record<string, unknown>), ...sharedPolicy },
      ...('media_source_channel_id' in patch ? { media_source_channel_id: patch.media_source_channel_id } : {}),
    } }
    if (patch.logic != null && edges.some(edge => edge.source === id
      && edge.target === node.id && edge.targetHandle === 'report-in'))
      return { ...node, data: { ...node.data, logic_name: String(patch.logic) } }
    return node
  })
}
