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
  "profile_id": "factory",
  "contract_id": "object_invade_det",
  "media": ["annotated_image", "raw_image"],
  "when": {
    "event_types": ["person_intrusion"]
  }
}
```

- `profile_id`：上传服务 `config.yaml` 中的连接。
- `contract_id`：上传服务 `contracts/*.json` 中的接口模板。
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

实现入口是 `event_report.h/.cpp`，单元测试位于
`tests/test_event_report/event_report_unit_test.cpp`。
