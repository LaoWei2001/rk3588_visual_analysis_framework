# logic_wafer2 — 晶圆轨迹分段可视化（visualize_trajectories.py 移植版）

- **上报**：无（纯画面可视化）
- **可调参数**：无（参数暂写死在代码里，出处见下）
- **用到的能力**：传统视觉（灰度/二值/形态学/凸包/fitEllipse）、跨帧状态、半透明折线、freetype 中文图例

## 做什么
把 `src/logic/visualize_trajectories.py`（离线全视频脚本）改成实时逐帧版。模型类别 0=cleanwiper(刮片)、1=wafer(晶圆盘)，与 `logic_wafer` 相同：

1. **晶圆椭圆**：对 wafer 框做 5 步法提取（裁剪+pad10 → 灰度 → 高斯模糊 → 暗区二值(阈值105) → 开/闭运算 → 碎片轮廓(面积>100) → 凸包 → fitEllipse），面积突变(±30%)剔除 + EMA 平滑(α=0.05)。画成**红色虚线椭圆**+「覆盖基准」。
2. **覆盖率**：500×500 归一化掩码累计刮片轨迹（圆点 r=w/3 + 连线 0.7w，刮片宽固定 16px），单位圆内占比即覆盖率。
3. **轨迹分段**（脚本 `segment_trajectory` 逐行移植）：时间间隙(≥0.15s)预切 → 滑窗折返率判 横擦(linear)/环擦(spiral) → 长直线段按重平滑 PCA 角度突变再切 → 相邻短段合并 → 边界 spiral 复判。每新增 5 个点重算一次。
4. **叠加**：绿色半透明覆盖带（全程轨迹粗折线）、当前点（段色实心圆+白圈）、最近 3s 分段彩色轨迹、顶部图例（覆盖面积/刮片宽度 + 横擦/竖擦/环擦 槽位 ▶✓○，橙标签绿数值，640 宽下排两行）。

与脚本的实时化差异：刮片消失超 5s 自动复位开始下一轮（脚本单视频离线无需复位）；脚本用全视频中值椭圆做覆盖基准，实时版用当前 EMA 椭圆；YOLO 框由框架固定绘制（仅改色：刮片绿/晶圆灰）。

## 写死参数的出处
| 参数 | 值 | 来源 |
|---|---|---|
| dark_threshold | 105 | 脚本 `--dark-thresh` 默认（config.yaml roi.dark_threshold=80 脚本未用） |
| smooth_alpha | 0.05 | config.yaml `roi.smooth_alpha` |
| trail_sec | 3.0 | 脚本 `--trail-sec` 默认 |
| 刮片宽度 | 16px | 脚本 `compute_coverage_from_ellipse` 固定值 |
| mask_res | 500 | 脚本固定值 |
| RESET_MS / MAX_PTS / SEG_EVERY_PTS | 5000 / 4000 / 5 | 实时化新增 |

config.yaml 的 `tracking`/`sop` 两节属其它脚本，`visualize_trajectories.py` 不读取；`detector` 节(conf=0.25)对应通道模型配置 → 该通道 `obj_thresh` 设 0.25。

## 实现位置
代码在 `src/logic/logic_wafer2.cpp` 的 `logic_wafer2` 及 `w2_*` 辅助函数（`w2_reversal_rate` 折返率、`w2_pca_angle_smoothed` PCA 角、`w2_segment_trajectory` 分段、`w2_extract_ellipse` 椭圆提取），跨帧状态 `Wafer2State`（轨迹/椭圆/覆盖掩码/分段缓存）。整个逻辑及其全部 `w2_*` helper、`Wafer2State` 都自包含在这一个文件里。

## 接线
- 文件 / 注册：独立文件 `src/logic/logic_wafer2.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_wafer2", logic_wafer2);`。
- logics.json：`{ "name": "logic_wafer2", "label": "晶圆轨迹分段可视化(py移植)", "params": [] }`
- 通道模型须为 class0=cleanwiper、class1=wafer 的晶圆模型（与 Python 同款权重转 rknn 效果最接近），`obj_thresh: 0.25`，无需画 ROI。

## 复用提示
"逐帧采集轨迹 + 周期性对整条轨迹跑离线分析算法（节流重算）"的范本；也演示了用 `draw_line` 隔段画虚线椭圆、用半透明 `draw_polyline` 近似掩码叠加。要把参数开放到网页，按 `adding-config-parameter.md` 把 dark_threshold/trail_sec 等做成 ChannelConfig 字段即可。
