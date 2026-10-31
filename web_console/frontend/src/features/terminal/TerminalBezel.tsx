import { bezelMaterialGradients } from './terminalBezelMaterial'
import { BEZEL_JOINT_WIDTH, SCREEN_RECESS, bezelFaces, bezelJoints } from './terminalBezelGeometry'

/** The liner occupies only the gaps exposed by the curved glass above it. */
export default function TerminalBezel({ width, height, amount, id, inset = SCREEN_RECESS }: {
  width: number; height: number; amount: number; id: string; inset?: number
}) {
  const faces = bezelFaces(width, height, amount, inset)
  const joints = bezelJoints(width, height, amount, inset).map(([[x, y], [endX, endY]]) => `M${x},${y}L${endX},${endY}`).join(' ')
  const gradients = bezelMaterialGradients(width, height, amount, inset)
  return <>
    <svg className="term-bezel-material" viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none" aria-hidden="true">
      <defs>
        {gradients.map(({ face, outer, inner, ...coordinates }) => <linearGradient key={face}
          id={`${id}-${face}`} gradientUnits="userSpaceOnUse" {...coordinates}>
          <stop offset="0" stopColor={outer} />
          <stop offset=".12" stopColor={outer} />
          <stop offset="1" stopColor={inner} />
        </linearGradient>)}
      </defs>
      {Object.entries(faces).map(([face, points]) => <polygon key={face}
        points={points.map(point => point.join(',')).join(' ')} fill={`url(#${id}-${face})`} />)}
    </svg>
    <svg className="term-bezel-joints" viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none" aria-hidden="true">
      {/* A rounded recess and a small bevel lip; kept above the reflection blur. */}
      <path d={joints} fill="none" stroke="#10140d" strokeOpacity=".35" strokeWidth="5" />
      <path d={joints} transform="translate(1.2 .3)" fill="none" stroke="#b1b29a" strokeOpacity=".36" strokeWidth="1" />
      <path d={joints} fill="none" stroke="#10140e" strokeWidth={BEZEL_JOINT_WIDTH} />
    </svg>
  </>
}
