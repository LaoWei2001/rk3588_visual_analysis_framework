# logic_upload — 通用上报框架演示

- 源码：`rk3588_yolo/src/logic/modules/logic_upload/logic.cpp`
- 告警类型：`upload_demo_roi_entry`
- 触发：任一目标进入任一已配置 ROI 时触发一次；所有目标离开全部 ROI 后复位
- 状态：`UploadDemoState::latched`

这是实现真实 ROI 进入告警的首选业务模板，展示了 ROI 选择、闩锁、上报专用叠加、运行时字段和统一告警入口。若目的是先学习“一次调用多投递”、五种 DrawCommand Target、原始/叠加图片与视频，请先运行同目录文档 `logic_upload_teach.md` 对应的教学模块；本例重点放在真实报警条件。

## 核心流程

1. 没有 ROI 时提示先配置区域并保持未锁存；
2. 遍历 `ctx->results`，用 `ctx->roi_index_of(result.box)` 找到落区目标；
3. 多个目标同时落区时选择置信度最高者；
4. 用 `DrawCommand::UPLOAD` 增加只供告警媒体使用的文字和红框；
5. 第一次命中调用 `report_alarm()`，后续帧由闩锁抑制；
6. 全部目标离开 ROI 后解除闩锁，允许下一次进入再次告警。

当前调用字段：

```cpp
const std::string event_id = report_alarm(
    ctx,
    "upload_demo_roi_entry",
    "检测到目标进入ROI",
    {
        alarm_field("label", best->label),
        alarm_field("score", best->score),
        alarm_field("track_id", best->track_id),
        alarm_field("roi_index", best_roi),
        alarm_field("roi_name", roi_name),
        alarm_field("box_x", best->box.x),
        alarm_field("box_y", best->box.y),
        alarm_field("box_width", best->box.width),
        alarm_field("box_height", best->box.height),
    });
```

上述九个 key 已在模块 `logic.json.report_fields` 中按相同名称和类型声明，并会聚合到 Web 清单，因而可在 Dify 上报节点中映射。服务器图片投递当前使用固定 JSON，不消费这些算法字段。

## 画布接线

1. 通道连接视频源、模型、ROI 和 `logic_upload`；
2. 从 logic 节点连接一个或多个上报节点；
3. 每个节点选择图片→服务器、图片→Dify 或视频→Dify；
4. 地址、密钥和 Profile 在“服务配置”中维护；
5. 保存后检查通道配置的 `report_policy.deliveries`，不要查找通道级 URL 或上报开关。

没有有效 delivery 时 `report_alarm()` 返回空字符串，`logic_upload` 会继续保持业务闩锁，但不会生成事件目录。
