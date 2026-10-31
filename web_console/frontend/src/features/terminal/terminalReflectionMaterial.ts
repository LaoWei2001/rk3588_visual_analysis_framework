export const TERMINAL_REFLECTION_MATERIALS = {
  rough: { label: '粗糙塑料', samples: 48, weightScale: 24, maxEdge: 384, radius: 2, sigma: .75 },
  // Ideal smooth dielectric over an absorbing substrate: Fresnel reflection,
  // with the transmitted energy absorbed. No artificial diffuse contribution.
  smooth: { label: '光滑涂层', samples: 4, weightScale: 4, maxEdge: 768, radius: 0, sigma: 1 },
} as const
export type TerminalReflectionMaterial = keyof typeof TERMINAL_REFLECTION_MATERIALS
export const resolveReflectionMaterial = (value: unknown): TerminalReflectionMaterial => value === 'smooth' ? 'smooth' : 'rough'
