// 记住每个程序在编辑器里「最近编辑 / 使用」的配置文件名（basename，如 config_a.json）。
// 用途：从画布编辑器返回「程序管理」后，启动配置下拉自动选中刚编辑的那一份，免去再选一次。
// 存在浏览器 localStorage，按程序名归档；与后端的 active_config(上次「启动」所用) 互不影响、互为兜底。

const KEY = 'apps_last_config'
type CfgMap = Record<string, string>

/** 读取整张「程序 → 最近配置文件名」映射（解析失败/不可用时返回空表）。 */
export function getLastConfigMap(): CfgMap {
  try {
    const raw = localStorage.getItem(KEY)
    const v = raw ? JSON.parse(raw) : null
    return v && typeof v === 'object' ? (v as CfgMap) : {}
  } catch {
    return {}
  }
}

/** 记下某程序最近编辑/使用的配置文件名（传 basename，如 config_a.json）。 */
export function setLastConfig(appName: string, configBase: string): void {
  if (!appName || !configBase) return
  try {
    const m = getLastConfigMap()
    m[appName] = configBase
    localStorage.setItem(KEY, JSON.stringify(m))
  } catch {
    /* localStorage 不可用：忽略，下拉退回后端默认即可 */
  }
}
