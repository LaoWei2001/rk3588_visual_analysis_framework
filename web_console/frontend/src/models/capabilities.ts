// UI capabilities and offline fallback only. Available model types come from
// /console/info; RKNN tensor compatibility is validated by the backend/runtime.
export const MODEL_CAPABILITIES = {
  yolov5: { labelRequired: true, depth: false, relativeDepth: false },
  yolov8_obb: { labelRequired: false, depth: false, relativeDepth: false },
  yolov8_det: { labelRequired: true, depth: false, relativeDepth: false },
  yolov8_pose: { labelRequired: false, depth: false, relativeDepth: false },
  yolo26_pose: { labelRequired: false, depth: false, relativeDepth: false },
  yolo26_depth: { labelRequired: false, depth: true, relativeDepth: false },
  zip_depth: { labelRequired: false, depth: true, relativeDepth: true },
  yolov5_seg: { labelRequired: true, depth: false, relativeDepth: false },
} as const

export const FALLBACK_MODEL_TYPES = Object.keys(MODEL_CAPABILITIES)

export function modelCapabilities(modelType: string) {
  return Object.prototype.hasOwnProperty.call(MODEL_CAPABILITIES, modelType)
    ? MODEL_CAPABILITIES[modelType as keyof typeof MODEL_CAPABILITIES]
    : undefined
}

export const isDepthModel = (modelType: string) => modelCapabilities(modelType)?.depth === true
export const requiresModelLabels = (modelType: string) => modelCapabilities(modelType)?.labelRequired === true
