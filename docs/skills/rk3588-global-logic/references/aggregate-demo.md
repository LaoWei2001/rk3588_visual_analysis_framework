# 当前多通道人数聚合示例

仓库当前最小的多通道聚合与上报闭环由以下模块组成：

```text
通道 0/1/...：logic_roi_person_count_demo
  publish person_count: integer
                 │
                 ▼
全局实例：global_person_count_alarm_demo
  汇总人数 → 阈值闩锁 → person_count_alarm → report_event()
```

另一个已注册的全局模块是 `global_crane_safety_controller`，用于行车业务联动；它不是人数聚合
示例。历史名称 `logic_global_input_demo`、`global_channel_aggregate_demo`、`global_default` 和
`global_two_channel_demo` 当前均不在源码模块目录中。

## 上游通道模块

`logic_roi_person_count_demo` 调用 `roi_count_target(ctx, "person", ROI_ALL)`，在每次通道业务帧发布
`person_count`。使用它前必须确认模型标签确实为 `person`，并在通道配置中提供业务需要的 ROI；
全局模块只读取公开 output，不读取检测框或上游私有 state。

## 全局模块当前行为

`global_person_count_alarm_demo` 遍历 `gctx->inputs()`：

1. 汇总每路 `person_count`；
2. 把人数大于 0 的通道加入 `request.evidence_channel_ids`；
3. 总人数首次达到 `alarm_threshold` 时创建 `person_count_alarm`；
4. 本地事件系统接受后设置 `reported=true`，条件持续成立期间不重复创建；
5. 总人数降到阈值以下后清除闩锁，允许下一次重新触发。

模块每次执行到人数汇总位置都会把当前统计写到标准输出，不判断人数是否变化，也不做时间限频。
格式为：

```text
[GlobalPersonCount][<instance_id>] 通道0:<人数>人,通道1:<人数>人,总人数<人数>人
```

通道编号使用配置中的真实 channel ID；没有可读输入时输出 `总人数0人`。输出后立即
`fflush(stdout)`，因此在设备终端、Web 日志页或 systemd journal 中可以及时看到。由于全局逻辑
由通道发布驱动，这会产生高频日志；这是该测试模块按业务要求保留的行为，生产模块应评估日志 I/O。

聚合事件不设置 `request.source_channel_id`，因此不存在代表通道或主通道。事件身份属于全局实例；
`evidence_channel_ids` 只描述本次告警涉及哪些通道，最终告警图片来源由 Web 上报节点决定。

模板 `http_person_count_alarm.json` 当前要求一张带标注图片，并映射 `event.type`、
`total_person_count` 和 `alarm_threshold`。`report.accepted()` 只表示请求进入本地持久化链路，不表示
媒体已经生成或远端已经收到。

## Web 配置要点

在画布中：

1. 每个参与通道选择 `logic_roi_person_count_demo`，核对模型标签并配置 ROI；
2. 把这些通道 Logic 连到 `global_person_count_alarm_demo`；
3. 配置总人数报警阈值；
4. 连接“上报配置”节点并选择当前模板和投递连接；
5. 在“告警图片来源”选择本次证据通道、指定通道或所有连入通道；
6. 只有模板要求事件视频时，才需要另外选择 `media_source_channel_id`。

“事件视频来源通道”只决定预录哪一路物理视频，不是事件主通道，也不参与图片选择。

## 调度注意事项

任一上游通道发布新人数后，全局实例会被立即唤醒；`poll_interval_ms` 只是无更新时的兜底周期。
回调不是固定频率，多路快速更新可能合并为一次最新状态计算。业务计时应使用
`gctx->timestamp_ms`/`gctx->dt_ms`，不能使用 tick 次数推算时间。

## 验收矩阵

| 场景 | 预期 |
|---|---|
| 所有上游未发布、离线或过期 | `inputs()` 为空，总数为 0，不告警 |
| 总人数未达阈值 | 清除闩锁，不告警 |
| 总人数首次达到阈值 | 创建一次全局事件；accepted 后闩锁 |
| 条件持续成立 | 不重复创建 |
| 总人数降到阈值以下后再次达到 | 重新创建一次事件 |
| 部分通道人数大于 0 | 这些通道进入 `evidence_channel_ids` |
| Web 选择“本次触发告警的通道” | 拼接证据通道，不选择主通道 |
| 上传失败 | 本地 outbox 保留并重试，不能记作远端成功 |

实际实现以 `vision_analysis/src/logic/modules/logic_roi_person_count_demo/` 和
`vision_analysis/src/logic/global_modules/global_person_count_alarm_demo/` 为准。
