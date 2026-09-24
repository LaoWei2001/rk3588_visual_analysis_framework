export const MODEL_ID_MAX_LENGTH = 64

const MODEL_ID_PATTERN = /^[a-z][a-z0-9_-]*$/

/**
 * 模型 ID 是通道内的稳定业务标识，同时作为 OTA model_id。
 * 限制为适合配置、日志、URL 和临时文件名的安全 ASCII 子集。
 */
export function validateModelId(value: unknown): string | null {
  const id = String(value ?? '').trim()
  if (!id) return '模型业务 ID 不能为空'
  if (id.length > MODEL_ID_MAX_LENGTH) return `模型业务 ID 不能超过 ${MODEL_ID_MAX_LENGTH} 个字符`
  if (!MODEL_ID_PATTERN.test(id)) {
    return '只允许小写字母、数字、下划线和短横线，且必须以字母开头'
  }
  return null
}
