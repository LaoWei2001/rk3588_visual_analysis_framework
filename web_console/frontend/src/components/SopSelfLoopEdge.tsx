import { EdgeProps, BaseEdge, EdgeLabelRenderer, getBezierPath, Position } from '@xyflow/react'
import type { CSSProperties } from 'react'

// SOP 子画布的"智能"边组件 — 根据 source/target 相对位置自动选路径:
//   ① self-loop (source === target)      → 节点上方画一个圆形弧
//   ② 反向边 (target 在 source 左边)      → 上方画一个圆形弧绕过节点
//   ③ 前向边 (默认 / target 在 source 右边)→ 沿用 ReactFlow 默认贝塞尔
// 所有路径都是单条三次贝塞尔(C 命令),没有直线段插入 → 弧顶始终圆润, 不会出现"平的一段"。
//
// 名字保留 SopSelfLoopEdge 以避免到处改 import; 实际用途已是通用 step↔step 边。
export default function SopSelfLoopEdge(props: EdgeProps) {
  const {
    id, source, target,
    sourceX, sourceY, targetX, targetY,
    sourcePosition, targetPosition,
    label, labelStyle, labelBgStyle, markerEnd, style, selected,
  } = props

  const selfLoop = source === target
  // 反向判定: target 比 source 左 30+px (排除自环抖动 + 节点位置接近的前向)
  const reverse  = !selfLoop && (targetX < sourceX - 30)

  let path = ''
  let labelX = 0
  let labelY = 0

  if (selfLoop) {
    // ① 自环: 单条三次贝塞尔, 两个控制点拉到节点上方两侧 → 形成扁圆顶弧
    const ax = sourceX, ay = sourceY
    const bx = targetX, by = targetY
    const peakY = ay - 70                                                    // 顶部抬升减少: 弧更扁
    // 控制点横向偏 60px (原 100), 减少左右铺开 → 弧形更紧凑
    path = `M ${ax} ${ay} C ${ax + 60} ${peakY}, ${bx - 60} ${peakY}, ${bx} ${by}`
    // 实际贝塞尔顶点 y ≈ 0.25*ay + 0.75*peakY, 标签贴近顶点上方 6px
    labelX = (ax + bx) / 2
    labelY = 0.25 * ay + 0.75 * peakY - 6
  } else if (reverse) {
    // ② 反向边: 单条三次贝塞尔, 两个控制点位于 source/target 各自正上方
    // 起点切线垂直向上、终点切线垂直向下, 中间对称圆弧, 无平段。
    const dx = sourceX - targetX
    const arcHeight = Math.max(50, dx * 0.28)        // 扁圆: 高度只占距离的 28%
    const baseY = Math.min(sourceY, targetY)
    const peakY = baseY - arcHeight
    path = `M ${sourceX} ${sourceY} C ${sourceX} ${peakY}, ${targetX} ${peakY}, ${targetX} ${targetY}`
    labelX = (sourceX + targetX) / 2
    // 贝塞尔顶点 y ≈ 0.25*baseY + 0.75*peakY, 标签紧贴上方 6px
    labelY = 0.25 * baseY + 0.75 * peakY - 6
  } else {
    // ③ 前向: 默认贝塞尔
    const [bezPath, bezLabelX, bezLabelY] = getBezierPath({
      sourceX, sourceY, targetX, targetY,
      sourcePosition: sourcePosition ?? Position.Right,
      targetPosition: targetPosition ?? Position.Left,
    })
    path = bezPath
    labelX = bezLabelX
    labelY = bezLabelY
  }

  const finalStyle: CSSProperties = {
    fill: 'none',
    ...(style as CSSProperties),
    strokeWidth: selected ? 4 : ((style as CSSProperties)?.strokeWidth ?? 2),
  }

  return (
    <>
      <BaseEdge id={id} path={path} markerEnd={markerEnd} style={finalStyle} />
      {label && (
        <EdgeLabelRenderer>
          <div
            style={{
              position: 'absolute',
              transform: `translate(-50%, ${selfLoop || reverse ? '-100%' : '-50%'}) translate(${labelX}px, ${labelY}px)`,
              pointerEvents: 'all',
              fontWeight: 600,
              fontSize: 11,
              padding: '3px 8px',
              borderRadius: 4,
              color: (labelStyle as { fill?: string })?.fill ?? '#fef3c7',
              background: (labelBgStyle as { fill?: string })?.fill ?? '#7c2d12',
              border: `1px solid ${(labelBgStyle as { stroke?: string })?.stroke ?? '#f59e0b'}`,
              whiteSpace: 'nowrap',
            }}
            className="nodrag nopan"
          >
            {label}
          </div>
        </EdgeLabelRenderer>
      )}
    </>
  )
}
