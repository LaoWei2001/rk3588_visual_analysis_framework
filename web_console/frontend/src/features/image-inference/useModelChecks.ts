import { useEffect, useState } from 'react'
import type { ModelInfo } from '../../api/apps'
import { apiErrorMessage } from '../../api/http'
import { inspectImageModel, type ImageModel } from '../../api/imageInference'

export interface ModelCheck { info?: ModelInfo; error?: string }

export default function useModelChecks(models: ImageModel[]) {
  const scope = JSON.stringify(models.map(model => [model.model_path, model.model_type]))
  const [revision, setRevision] = useState(0)
  const [state, setState] = useState<{ scope: string; revision: number; checks: ModelCheck[] }>({ scope: '', revision: 0, checks: [] })
  useEffect(() => {
    const controller = new AbortController()
    setState({ scope, revision, checks: [] })
    // Coalesce rapid model/type changes before starting the native check.
    const timer = window.setTimeout(() => {
      const selections: [string, string][] = JSON.parse(scope)
      selections.forEach(async ([path, type], index) => {
        if (!path) return
        let check: ModelCheck
        try {
          const info = await inspectImageModel(path, type, controller.signal)
          check = info.ok ? { info } : { error: info.error || '模型检查失败' }
        } catch (error) { check = { error: apiErrorMessage(error) } }
        if (!controller.signal.aborted) setState(current => {
          if (current.scope !== scope || current.revision !== revision) return current
          const checks = [...current.checks]
          checks[index] = check
          return { ...current, checks }
        })
      })
    }, 200)
    return () => { window.clearTimeout(timer); controller.abort() }
  }, [scope, revision])
  // Invalidate synchronously on selection changes, before the effect runs.
  const checks = state.scope === scope && state.revision === revision ? state.checks : []
  const ready = models.length > 0 && models.every((model, index) => model.model_path && checks[index]?.info?.ok)
  return { checks, ready, retry: () => setRevision(value => value + 1) }
}
