# `src/event`：标准事件与媒体生成

权威 API：`vision_analysis/src/event/event_report.h`。

```cpp
EventReportResult report_event(ChannelContext *ctx, const EventRequest &request);
```

事件只通过 `EventRequest` 提交，合并策略只有通用
`EventMergeMode::POLICY/NEVER`。不存在 server、Dify、SOP 或纯 JSON 专用参数。

C++ 职责：

- 按 `when.event_types` 过滤 delivery；
- 从匹配 delivery 的 `media[]` 推导媒体需求；
- 持久化 schema v3 标准事件；
- 异步生成 `annotated.jpg`、`raw.jpg` 和 `clip.mp4`；
- 维护媒体 requested/generating/ready/failed；
- 按策略合并同通道同类型事件；
- 控制 `event_store` 容量。

C++ 不解释 `profile_id/contract_id`，只读取必要快照中的 `media[]`，
并把匹配 delivery 原样写进 `delivery_state.json`。

发件箱：

```text
event_store/<event_id>/
├── event.json
├── media_state.json
├── delivery_state.json
├── annotated.jpg
├── raw.jpg
└── clip.mp4
```

测试：`vision_analysis/tests/test_event_report/event_report_unit_test.cpp`。
