# 通用事件上报

本目录提供视觉引擎唯一的事件提交入口。logic 只描述“发生了什么”，不选择 HTTP、Dify、
图片或视频，也不读取连接地址和密钥。

## logic 最小用法

```cpp
EventRequest event;
event.event_type = "person_intrusion";
event.message = "有人进入危险区域";
event.fields = {
    event_field("person_count", count),
    event_field("region", "danger_zone"),
    event_field("score", score),
};
const EventReportResult result = report_event(ctx, event);

if (!result.accepted())
    fprintf(stderr, "event rejected: %s\n", result.detail.c_str());
```

JSON 对象或数组使用 `event_json_field()`。需要禁止事件合并时设置：

```cpp
EventRequest event;
event.event_type = "inspection_result";
event.message = "本轮巡检完成";
event.merge_mode = EventMergeMode::NEVER;
event.fields.set_json("result", result_json);
const EventReportResult result = report_event(ctx, event);
```

`accepted()` 只表示事件进入了本地持久化发件箱，不表示远端已经收到。

## 全局事件的图片通道

全局 logic 如果知道本次事件由哪些通道触发，只需额外提交业务证据 ID：

```cpp
EventRequest event;
event.event_type = "person_fall_into_water";
event.evidence_channel_ids = alarm_channel_ids;
report_event(gctx, event);
```

这不会在 C++ 中写死最终图片选择。Web 上报节点把选择保存到
`report_policy.image_selection`，底层统一取帧和拼接：

```json
{
  "image_selection": {
    "mode": "selected",
    "channel_ids": [0, 2]
  }
}
```

- `event_evidence`：使用 `EventRequest.evidence_channel_ids`；旧逻辑未提供时使用全部连入通道。
- `selected`：使用 Web 固定选择的 `channel_ids`。
- `connected`：使用画布连入全局 logic 的全部通道，兼容旧配置。

全局聚合事件没有“主通道”。`source_channel_id` 只用于确实由单路通道独立触发的
全局业务事件，不参与图片或视频选择，也不会由框架自动回退生成。

取帧失败的通道会被跳过；至少一路可用就继续生成紧凑拼图。全部不可用时事件仍会
持久化，图片状态明确标记为失败。事件的 `source.image_channel_ids` 和
`source.missing_image_channel_ids` 记录实际结果，上传适配器仍只处理一份
`annotated_image` / `raw_image`，无需了解拼图细节。

## 稳定边界

```text
logic
  -> EventRequest
  -> report_event()
  -> event_store/<event_id>/
       event.json
       media_state.json
       delivery_state.json
       annotated.jpg / raw.jpg / clip.mp4（按需）
  -> Python EventOutboxForwarder
  -> delivery adapter
```

`report_policy.deliveries[]` 使用统一结构：

```json
{
  "id": "factory_http",
  "enabled": true,
  "connection_id": "factory",
  "contract_id": "logic_example.object_invade_det",
  "contract_revision": "sha256-content-revision",
  "media": ["annotated_image", "raw_image"],
  "when": {
    "event_types": ["person_intrusion"]
  }
}
```

`report_policy.enabled=false` 是整条事件链的总开关。关闭时 `report_event()` 在创建本地事件前
直接返回 `DISABLED`，不会写入告警箱，也不会进入上传队列；`deliveries[]` 可以继续保留，供画布
重新开启时恢复原配置。多个 delivery 可通过各自的 `enabled` 独立开关。

- `connection_id`：当前程序 `.data/<app>/connections.yaml` 中的投递连接。
- `contract_id`：包内模板或当前程序自定义契约 ID。
- `contract_revision`：契约内容 revision，保证积压事件始终按创建时的请求格式投递。
- `media`：可选 `annotated_image`、`raw_image`、`video`；空数组表示仅事件数据。
- `when.event_types`：可选事件类型过滤；空或缺省表示匹配全部事件。

C++ 只扫描匹配 delivery 的 `media[]`，据此请求图片或视频。adapter 由接口契约唯一决定；
C++ 不包含任何 server、Dify、
SOP 或客户协议分支。

## 媒体状态

```text
requested -> generating -> ready
                        \-> failed
```

带标注图和原图共用 `image` 生成状态，但文件键分别是 `annotated_image` 和 `raw_image`；
视频文件键为 `video`。无媒体事件的总状态直接为 `ready`。

环境变量：

- `EVENT_STORE_DIR`
- `EVENT_STORE_MAX_BYTES`
- `EVENT_STORE_MIN_FREE_BYTES`

实现入口是 `event_report.h/.cpp`。当前仓库没有独立的 C++ 事件模块单元测试；
上报队列、契约选择和重试行为由 `service/upload/tests/test_functional.py` 覆盖，
改动本目录代码时仍需在设备侧补做事件生成、媒体落盘和断网恢复验证。
