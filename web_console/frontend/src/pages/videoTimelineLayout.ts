export interface VideoTile {
  channelId: number
  column: number
  row: number
  left: number
  top: number
  width: number
  height: number
}

export interface VideoLayout {
  width: number
  height: number
  columns: number
  rows: number
  tiles: VideoTile[]
}

function positiveInteger(value: unknown, fallback: number): number {
  const number = Number(value)
  return Number.isFinite(number) && number >= 1 ? Math.floor(number) : fallback
}

/** 与引擎的通道排序、自动扩行和 RGA 对齐规则一致。 */
export function videoTimelineLayout(config: Record<string, unknown>): VideoLayout {
  const global = (config.global ?? config) as Record<string, unknown>
  const width = Math.max(4, Math.floor(positiveInteger(global.disp_width, 1920) / 4) * 4)
  const height = Math.max(2, Math.floor(positiveInteger(global.disp_height, 1080) / 2) * 2)
  const columns = positiveInteger(global.tile_cols, 2)
  const channels = Array.isArray(config.channels) ? config.channels : []
  const channelIds = channels.flatMap((value, index) => {
    if (!value || typeof value !== 'object') return []
    const channel = value as Record<string, unknown>
    const stream = (channel.stream ?? {}) as Record<string, unknown>
    const enabled = channel.enable === undefined || Boolean(channel.enable)
    const location = stream.src_type === 'usb' ? stream.device : stream.url
    const id = channel.id === undefined ? index : Number(channel.id)
    return enabled && typeof location === 'string' && location.length > 0 && Number.isInteger(id) && id >= 0
      ? [id] : []
  }).sort((a, b) => a - b)
  const rows = Math.max(positiveInteger(global.tile_rows, 2), Math.ceil(channelIds.length / columns))
  const cellWidth = Math.floor(width / columns)
  const cellHeight = Math.floor(height / rows)
  const tiles = channelIds.map((channelId, index) => {
    const column = index % columns
    const row = Math.floor(index / columns)
    const left = Math.floor(column * cellWidth / 4) * 4
    const top = Math.floor(row * cellHeight / 2) * 2
    const right = column === columns - 1 ? width : Math.floor((column + 1) * cellWidth / 4) * 4
    const bottom = row === rows - 1 ? height : Math.floor((row + 1) * cellHeight / 2) * 2
    return { channelId, column, row, left: left / width, top: top / height,
      width: (right - left) / width, height: (bottom - top) / height }
  })
  return { width, height, columns, rows, tiles }
}
