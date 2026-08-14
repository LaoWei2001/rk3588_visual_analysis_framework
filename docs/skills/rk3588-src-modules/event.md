# `src/event`：标准事件与媒体生成

权威 API：`vision_analysis/src/event/event_report.h`。

```cpp
EventReportResult report_event(ChannelContext *ctx, const EventRequest &request);
EventReportResult report_event(GlobalContext *ctx, const EventRequest &request);
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

`report_event()` 在调用线程内创建事件目录并原子写入 `delivery_state.json`、`media_state.json`、
`event.json`（最后一个也是目录可见标记）；图片渲染/编码和事件录像在后台 worker 完成。

通道调用使用当前通道帧；全局调用用 `request.source_channel_id`（未设置时取全局节点默认来源或
首个输入）选择来源标识和事件视频通道，告警图片则按全局显示尺寸拼接该实例的全部输入通道。
这里的 `raw.jpg` 表示“未叠加图片”，不是摄像头原分辨率文件：通道事件使用未叠加的模型输入帧，
全局事件使用未叠加的多窗格拼图。事件视频的原始模式才从 recorder 的解码源分辨率帧生成。

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

当前仓库没有独立的 C++ event 单元测试目录。现有可自动执行的相关测试是
`service/upload/tests/test_functional.py` 和 `web_console/backend/tests/test_records_split_event.py`；
C++ 侧还必须通过模块清单静态校验，并在目标设备用真实事件验证三份状态文件和图片/视频终态。
