# 当前多通道聚合示例

仓库当前唯一完整的多通道闭环由以下模块组成：

```text
通道 0/1/...：logic_global_input_demo
  publish target_count: integer
  publish local_alarm: boolean
  publish risk_ratio: number
                 │
                 ▼
全局实例：global_channel_aggregate_demo
  聚合 → 闩锁 → channel_aggregate_alarm → report_event()
```

另一个已注册全局模块是空操作 `global_default`。当前不存在历史名称
`global_two_channel_demo`。

## 上游通道模块

`logic_global_input_demo` 的参数：

- `channel_role`：通道业务角色；
- `target_label`：必须与模型标签完全一致；
- `local_count_threshold`：局部阈值。

每业务帧发布 `target_count`、`local_alarm`、`risk_ratio`，且 manifest 声明了匹配类型。全局模块
不读取上游的私有 state，也不重复解析检测框。

## 全局模块当前行为

`global_channel_aggregate_demo` 遍历 `gctx->inputs()`：

1. 求 `target_count` 总数、有效输入数和 `local_alarm` 通道数；
2. 选择 `risk_ratio` 最大的输入 ID 作为本次 `request.source_channel_id`；
3. 总数达到 `total_count_threshold`，且可选的 `require_local_alarm` 条件满足时触发；
4. 第一次被本地事件系统接受后设置 `reported=true`；条件清除后重新布防；
5. `reset_report` Action 可立即清除闩锁，若条件仍满足，下一个 tick 再创建事件。

事件 ID 是 `channel_aggregate_alarm`。manifest 同时声明 HTTP 模板和以下动态字段：
`server_event_type`、`valid_channel_count`、`alarm_channel_count`、`total_count`、`invade_flag`、
`yuv_width`、`yuv_height`、`yuv_flag`。

`report.accepted()` 使闩锁置位，但这只说明请求获得 CREATED/MERGED/CREATED_MEDIA_FAILED 之一，
不是远端已投递。

## Web 配置要点

在画布中：

1. 每个参与通道选择 `logic_global_input_demo` 并配置正确标签/阈值；
2. 把这些通道 logic 连到 `global_channel_aggregate_demo`；
3. 配置全局阈值；
4. 若要投递，连接“上报配置”节点并选择当前模板/连接；
5. 需要事件视频时必须选择 `media_source_channel_id`。

当前 `request.source_channel_id` 可动态改变事件身份和图片回退来源，但不会动态切换视频预录器；
视频来源始终是全局配置中的 `media_source_channel_id`。

注意：上游 `logic_global_input_demo/logic.json` 的 `risk_ratio.help` 仍声称该值会动态选择“事件来源和
视频通道”。后半句是过期元数据；当前 `global_channel_aggregate_demo/logic.cpp` 只把最高 ratio
通道写入 `request.source_channel_id`，事件模块仍固定按 `media_source_channel_id` 取视频。

## 验收矩阵

| 场景 | 预期 |
|---|---|
| 所有上游未发布/离线/过期 | `inputs()` 为空，不告警 |
| 总数未达阈值 | 清除闩锁，不告警 |
| 达阈值且首次触发 | 创建一次事件；accepted 后闩锁 |
| 条件持续成立 | 不重复创建 |
| 点击 `reset_report` 且条件仍成立 | 下一 tick 可再次创建 |
| 高风险通道变化 | 新事件的动态 `source_channel_id` 随最高 ratio 改变 |
| 上传失败 | 本地 outbox 保留/重试；不能记作远端成功 |

实际实现以
`vision_analysis/src/logic/modules/logic_global_input_demo/` 和
`vision_analysis/src/logic/global_modules/global_channel_aggregate_demo/` 为准。
