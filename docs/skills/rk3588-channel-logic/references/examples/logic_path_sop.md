# logic_path_sop — 目标路径/SOP 合规检测

- 源码：`vision_analysis/src/logic/modules/logic_path_sop/logic.cpp`
- 配置入口：Web SOP 子画布
- 注册：`REGISTER_LOGIC(logic_path_sop)` 和 `REGISTER_LOGIC_ACTION(logic_path_sop, ...)`

这是当前源码中完整度最高的业务 logic。它支持多命名 ROI、多个起点和终点、有向分支、汇合、回边、自环、每步进入防抖、停留上下限、整轮耗时限制、边循环次数限制和外部开始/结束触发。

## 当前告警类型

源码在一帧内先收集告警类型，完成所有叠加后再统一调用 `report_event(ctx, request)`：

- `sop_order_err`：进入了设计内区域，但不是当前候选的合法后继；
- `sop_missed`：结算时不存在已访问的有效入口到出口路径；
- `sop_dwell_short`：离开步骤或结算时停留不足；
- `sop_dwell_over`：步骤停留超过最大值；
- `sop_total_short` / `sop_total_over`：整轮总耗时越界；
- `sop_loop_violation`：受约束边的循环次数不符合范围；
- `sop_untracked_entry`：要求外部开始触发时，目标未经触发直接进入区域。

事件字段与模块 `logic.json.report_fields` 对齐，并由构建生成器聚合给 Web：

| key | 类型 | 含义 |
|---|---|---|
| `current_zone` | string | 当前区域 |
| `progress` | number | 已访问步骤进度 |
| `step_count` | number | 步骤总数 |
| `round_total_seconds` | number | 本轮总耗时 |
| `order_error` | boolean | 是否发生顺序错误 |
| `completed` | boolean | 是否到达合规完成状态 |

完整业务 JSON 的 `sop` 对象还会在正常和违规结果中固定输出两组循环数据：

- `configured_loop_edges`：画布中参与有向环/自环的边，以及显式配置次数范围的边；每项包含起止步骤、区域和 `required_min_count` / `allowed_max_count`，两端均为 `0` 表示任意次数；
- `actual_loop_counts`：本轮对应循环边的实际通过次数 `actual_count`，以及是否落在配置范围内的 `within_range`。

因此正常完成时也能直接读取实际循环次数，不再需要从 `zone_history` 推算；只有次数违规时，`alarm_type.loop_violation` 才会额外出现违规明细。

为了让上报结果可以完整还原本轮实际采用的 SOP 路径图，`sop` 还固定输出：

- `configured_edges`：实际生效的全部普通边、分支、回边和自环；
- `configured_entry_steps`：实际生效的入口步骤；
- `configured_exit_steps`：实际生效的出口步骤。

将这些字段与 `configured_sequence` 组合即可还原完整拓扑。边、入口和出口都是画布显式
配置，不存在隐式线性链或自动推断出口。

## 配置结构

SOP 的唯一配置入口是 `channels[].logic_parameters.flow`：

```json
{
  "logic": "logic_path_sop",
  "logic_parameters": {
    "flow": {
      "target_label": "person",
      "reset_sec": 5,
      "end_mode": "leave",
      "end_zone": "",
      "end_dwell_sec": 0,
      "total_min_sec": 0,
      "total_max_sec": 120,
      "trigger_mode": "auto",
      "trigger_mandatory": false,
      "report_normal": false,
      "steps": [
        {"zoneName": "入口", "enter_sec": 0.5, "dwell_min_sec": 0, "dwell_max_sec": 0},
        {"zoneName": "工位", "enter_sec": 0.5, "dwell_min_sec": 10, "dwell_max_sec": 60}
      ],
      "edges": [[0, 1]],
      "edge_limits": [],
      "entries": [0],
      "exits": [1]
    }
  }
}
```

`steps[].zoneName` 必须与 `roi_zones[].name` 完全一致。多个步骤时必须显式配置 `edges`；
`entries` 和 `exits` 至少各有一项。Web 保存前会检查这些条件，C++ 遇到不完整配置时只显示
配置错误，不猜测路线。`flow` 的热更新策略是 `reset_state`。

## 上报方式

SOP logic 不选择服务器或 Dify，也不拼装图片。画布连接的上报节点生成 `report_policy`；告警模块自动复用当前显示叠加生成 `annotated.jpg`，同时保留 `raw.jpg`，视频 delivery 则触发事件录像。

相同通道、相同告警类型在 `merge_window_sec` 内可能合并为同一事件。SOP 自身还按步骤/轮次去重，避免同一违规每帧重复提交。

## 动作

- `start_new_run`/`reset`：重置本轮状态；
- `sop_trigger`：外部开始触发；
- `sop_end_trigger`：外部结束触发。

动作处理只改变状态，真正的路径判断、绘制和告警仍在逐帧函数内完成。所有参数名和动作清单以当前 `logics.json` 与源码为准，不要从旧 `logic_wafer_sop` 文档迁移字段。
