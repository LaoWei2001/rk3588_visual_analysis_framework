// 记住每个程序在编辑器里「最后保存」的配置文件名（basename，如 config_copy.json），
// 供「程序管理」的启动配置下拉在返回后默认选中它 —— 纯前端 UI 偏好，存 localStorage。
const keyOf = (appName: string) => `last_config_${appName}`

/** 记录某程序最后保存的启动配置文件名（basename）。 */
export function saveLastConfig(appName: string, configBase: string): void {
  if (!appName || !configBase) return
  try { localStorage.setItem(keyOf(appName), configBase) } catch { /* ignore */ }
}

/** 读取某程序最后保存的启动配置文件名（basename）；没有则返回 null。 */
export function loadLastConfig(appName: string): string | null {
  try { return localStorage.getItem(keyOf(appName)) } catch { return null }
}
