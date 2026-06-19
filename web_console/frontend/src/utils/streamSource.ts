/**
 * 视频源输入类型 (src_type) 工具。
 *
 * 设计原则（与 C++ 端保持一致）：src_type 是【必填】字段，前后端都【不再自动推断】。
 *   - C++  : config.cpp 的 normalize_src_type() 仅做大小写归一；缺省即视为配置错误并拒绝加载。
 *   - 前端 : 这里只读取【显式】的 src_type，不再根据 url/device 猜测类型。
 *
 * 因此：新建视频流节点时必须显式带上 src_type（见 EditorPage 的默认节点数据），
 * 导入缺少 src_type 的旧配置时 UI 会提示"未指定 / 请选择"，而不是默认成 RTSP。
 */

export type SrcType = 'rtsp' | 'usb' | 'file'

/** 下拉选项（顺序即 UI 展示顺序）。供视频流节点的"输入类型"选择器复用。 */
export const SRC_TYPES: { value: SrcType; label: string }[] = [
  { value: 'rtsp', label: 'RTSP 摄像头' },
  { value: 'usb',  label: 'USB 摄像头' },
  { value: 'file', label: 'RK3588 本地文件' },
]

/**
 * 读取节点数据里【显式】的 src_type（小写、去首尾空白）。
 *
 * 不做任何基于 url/device 的推断：未指定时返回空串 ''，由 UI 提示用户必选、
 * 由后端配置校验拒绝。这样前端行为与 C++（src_type 必填、不推断）完全一致。
 */
export function getSrcType(d: Record<string, unknown> | null | undefined): string {
  if (!d) return ''
  return String(d.src_type ?? '').trim().toLowerCase()
}
