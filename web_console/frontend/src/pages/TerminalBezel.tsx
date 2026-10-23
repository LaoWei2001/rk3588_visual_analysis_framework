import { BEZEL_JOINT_WIDTH, SCREEN_RECESS, bezelFaces, bezelJoints } from './terminalBezelGeometry'

/** The liner occupies only the gaps exposed by the curved glass above it. */
export default function TerminalBezel({ width, height, amount, id }: {
  width: number; height: number; amount: number; id: string
}) {
  const faces = bezelFaces(width, height, amount, SCREEN_RECESS)
  const depthX = Math.max(1, SCREEN_RECESS + (width - SCREEN_RECESS * 2) / 2 * amount / (1 + amount))
  const depthY = Math.max(1, SCREEN_RECESS + (height - SCREEN_RECESS * 2) / 2 * amount / (1 + amount))
  const joints = bezelJoints(width, height, amount, SCREEN_RECESS).map(([[x, y], [endX, endY]]) => `M${x},${y}L${endX},${endY}`).join(' ')
  const gradients = [
    { face: 'top', x1: 0, y1: 0, x2: 0, y2: depthY, outer: '#66685b', inner: '#25281f' },
    { face: 'right', x1: width, y1: 0, x2: width - depthX, y2: 0, outer: '#55594c', inner: '#20251c' },
    { face: 'bottom', x1: 0, y1: height, x2: 0, y2: height - depthY, outer: '#777768', inner: '#35382d' },
    { face: 'left', x1: 0, y1: 0, x2: depthX, y2: 0, outer: '#4b5043', inner: '#1c211a' },
  ]
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
