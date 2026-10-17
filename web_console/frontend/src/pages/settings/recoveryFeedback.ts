export const readTimestamp = () => new Date().toLocaleTimeString('zh-CN', { hour12: false })
export const readSuccessMessage = (discarded: boolean, time = readTimestamp()) =>
  `已重新读取设备上的设置和最新状态（${time}）。${discarded ? '未保存的修改已放弃。' : ''}`
