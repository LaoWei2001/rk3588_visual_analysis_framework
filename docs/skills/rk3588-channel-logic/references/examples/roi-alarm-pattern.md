# ROI 进入告警代码模式

> 本文是可复制到自定义 `logic_xxx` 的业务模式，不是内置模块。当前可直接运行的上报示例是 `logic_upload_teach` 和 `logic_periodic_snapshot_demo`。

适用需求：任一目标进入任一已配置 ROI 时触发一次；所有目标离开全部 ROI 后复位。这个模式展示 ROI 选择、闩锁、上报专用叠加、运行时字段和统一告警入口。

## 核心流程

1. 没有 ROI 时提示先配置区域并保持未锁存；
2. 遍历 `ctx->results`，用 `ctx->roi_index_of(result.box)` 找到落区目标；
3. 多个目标同时落区时选择置信度最高者；
4. 用 `DrawCommand::MEDIA` 增加只供告警媒体使用的文字和红框；
5. 第一次命中调用 `report_event()`，后续帧由闩锁抑制；
6. 全部目标离开 ROI 后解除闩锁，允许下一次进入再次告警。

自定义模块可采用下面的调用字段：

```cpp
EventRequest request;
request.event_type = "upload_demo_roi_entry";
request.message = "检测到目标进入ROI";
request.fields = {
    event_field("label", best->label),
    event_field("score", best->score),
    event_field("track_id", best->track_id),
    event_field("roi_index", best_roi),
    event_field("roi_name", roi_name),
    event_field("box_x", best->box.x),
    event_field("box_y", best->box.y),
    event_field("box_width", best->box.width),
    event_field("box_height", best->box.height),
};
const EventReportResult report = report_event(ctx, request);
```

上述九个 key 还必须在自定义模块的 `logic.json.report_fields` 中按相同名称和类型声明。需要它们
的远端接口契约再通过 `fields.label`、`fields.score` 等 source 引用这些值。

## 画布接线

1. 创建并注册自己的 `logic_xxx`，把 ROI 判断、闩锁和上面的 `report_event()` 放入该模块；
2. 通道连接视频源、模型、ROI 和这个自定义 logic；
3. 从 logic 节点连接一个或多个“上报配置”节点；
4. 每个节点选择连接 Profile 和接口契约；契约自动决定所需媒体、固定值和字段 mapping；
5. 地址、密钥和 Profile 在“服务配置”中维护，修改后重启上报服务；
6. 保存后检查通道配置的 `report_policy.deliveries`，不要查找通道级 URL 或上报开关。

没有有效 delivery 时 `report_event()` 返回 `NO_DELIVERY`。业务上是否继续保持闩锁必须由需求
明确决定：如果希望“未配置上报时不算已经报过”，只在 `report.accepted()` 时锁存；如果希望
避免条件持续满足后反复尝试，则可以先锁存并通过 `status/detail` 提示配置问题。
