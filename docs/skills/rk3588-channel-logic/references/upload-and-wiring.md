# 标准事件上报与画布接线

## 开发者只做一件事

logic 判断事件成立后调用 `report_event()`：

```cpp
EventRequest event;
event.event_type = "person_intrusion";
event.message = "有人进入危险区域";
event.fields = {
    event_field("track_id", hit->track_id),
    event_field("score", hit->score),
    event_field("roi_name", roi_name),
};
const EventReportResult report = report_event(ctx, event);

if (!report.accepted())
    fprintf(stderr, "status=%s detail=%s\n",
            event_report_status_name(report.status), report.detail.c_str());
```

不要在 logic 中：

- 调 HTTP 或 Dify；
- 读取 URL、Token、API Key；
- Base64 编码图片；
- 选择上传目标；
- 自己保存上报图片或视频；
- 按不同目标重复提交同一个事件。

JSON 对象或数组使用 `event_json_field()`。每次触发必须独立建事件时：

```cpp
EventRequest event;
event.event_type = "inspection_round";
event.message = "巡检完成";
event.merge_mode = EventMergeMode::NEVER;
event.fields.set_json("result", result_json);
const EventReportResult report = report_event(ctx, event);
```

## 画布配置

一个上报节点对应一条 delivery：

1. 在“服务配置”中新建连接 Profile，并选择适配器；
2. logic 节点连接上报节点；
3. 上报节点选择 Profile 和接口模板；没有合适模板时点击“新建接口模板”；
4. 编辑器从当前 logic 的 `logic.json.report_fields` 自动列出算法变量，并同时提供系统事件、
   媒体和固定值；选择变量、填写远端字段名后保存；
5. 上报节点从 `logic.json.event_types` 显示可选事件类型；默认接收全部，确需分流时才勾选子集；
6. 模板自动带出媒体、固定值、字段 mapping 和成功条件；
7. 点击“预览请求”；真实联调时刷新并选择一条本地事件，再点击“测试发送”；
8. 新建或修改模板后重启事件投递服务，使运行中的发件箱重新加载模板。

同一个 logic 可连接多个上报节点。logic 仍然只调用一次 `report_event()`。

## delivery Schema

```json
{
  "id": "delivery_1",
  "enabled": true,
  "profile_id": "server_22",
  "contract_id": "jnu_alarm_upload",
  "media": ["annotated_image", "raw_image"],
  "when": {
    "event_types": ["person_intrusion"]
  }
}
```

`media` 是保存到 delivery 的必要快照，C++ 用它决定媒体任务。adapter、mapping、请求方式和
成功条件都不复制到 delivery；仓库默认契约位于 `service/upload/contracts/`，Web 管理的运行契约
位于 `/opt/ai_apps/.data/<App>/contracts/`，Python 以运行目录中的 `<contract_id>.json` 为协议权威。
`when.event_types` 缺省或空数组表示匹配所有事件。

事件类型不再由 Web 自由输入。每个 channel logic 的 `logic.json` 都必须声明：

```json
"event_types": [
  {"id": "person_intrusion", "label": "人员入侵", "help": "进入警戒区域时产生"}
]
```

`id` 必须与 C++ `EventRequest.event_type` 完全一致；不产生事件的 logic 显式声明空数组。
`generate_logics_catalog.py --check` 会拒绝调用了 `report_event()` 却没有声明事件类型的模块，
并校验 C++ 直接使用的事件类型字符串。

## 接口模板

接口模板是开发者和大模型维护服务器协议的地方。通常在 Web 上报节点中通过字段选择器创建
或编辑；保存结果写入 `/opt/ai_apps/.data/<App>/contracts/*.json`，同名 App 覆盖时保留。
需要代码评审、版本管理或分发给新 App 时，再把确认后的模板同步到仓库
`service/upload/contracts/`，也可由大模型直接审查修改：

仓库当前 `service/upload/contracts/server.json` 的 ID 是 `jnu_alarm_upload`，需要
`annotated_image + raw_image`；它的完整 mapping 以该文件为准。下面仅演示“新建另一份自定义
契约”时的结构，必须使用新的 ID，不能拿示意内容覆盖同名默认契约：

```json
{
  "id": "custom_object_invade",
  "label": "自定义入侵接口",
  "adapter": "http_json",
  "media": ["annotated_image", "raw_image"],
  "request": {"method": "POST"},
  "mapping": [
    {"source": "constant", "target": "source", "value": "JNU"},
    {"source": "fields", "target": "detResult"},
    {"source": "media.annotated_image", "target": "base64Data", "transform": "base64"}
  ],
  "success": {"http_status": [200]}
}
```

固定协议字段放模板；算法动态值仍由 C++ `EventRequest.fields` 提供。修改模板后重启
`unified_upload`。如果修改了模板的 `media[]`，还要在 Web 重新选择该模板并保存配置，
让 C++ 获得新的媒体快照。

Web 字段选择器展示的是声明，不是运行时数值：

```text
logic.json.report_fields -> logics.json -> Web 可选 fields.<key>
logic.cpp EventRequest.fields             -> 事件触发时的真实数值
```

新算法变量必须同时出现在两处；服务器只改字段名时只编辑模板的 target，不修改 C++。

## 稳定 source

| source | 内容 |
|---|---|
| `event.id` | 事件 ID |
| `event.type` | logic 提交的事件类型 |
| `event.message` | 事件说明 |
| `event.trigger_unix_ms` | 触发时间戳 |
| `source.channel_id` | 通道 ID |
| `source.parameters.<key>` | 通道 report_parameters |
| `fields.<key>` | logic 的自定义字段 |
| `media.annotated_image` | 带叠加图片文件 |
| `media.raw_image` | 未叠加图片文件；通道事件当前为模型输入尺寸，不是摄像头原分辨率 |
| `media.video` | MP4 文件 |
| `event/fields/source` | 整体对象 |
| `constant` | 映射条目中的固定 `value` |

`logic.json.report_fields[]` 只负责让 Web 知道 `fields.<key>` 的名称、类型和说明。它必须与
C++ `event_field()` 的 key 对齐。

## 媒体时序

图片和视频由 delivery 的 `media[]` 自动请求：

```text
requested -> generating -> ready
                        \-> failed
```

视频会等待事件前后窗口并异步编码。logic 不等待媒体完成。希望某个 `draw_*` 进入带标注图
时，必须先绘制，再调用 `report_event()`。

## 返回结果

| 状态 | 含义 |
|---|---|
| `CREATED` | 新事件已持久化 |
| `MERGED` | 合并进同通道同类型事件 |
| `CREATED_MEDIA_FAILED` | 事件已保存，但媒体请求失败 |
| `DISABLED` | report_policy 关闭 |
| `NO_DELIVERY` | 没有匹配当前事件类型的有效 delivery |
| `INVALID_REQUEST` | ctx/config/type 无效 |
| `WORKER_UNAVAILABLE` | 图片线程无法启动 |
| `STORAGE_ERROR` | 事件目录或状态文件写入失败 |

## 排错顺序

1. 检查 `EventReportResult.status/detail/event_id`；
2. 查看 `event_store/<event_id>/event.json`（Web 管理模式为 `/opt/ai_apps/.data/<App>/event_store/`）；
3. 查看 `media_state.json` 的 requested/generating/ready/failed；
4. 查看 `delivery_state.json` 的适配器、Profile、attempts 和 last_error；
5. 在 Web 上报节点预览最终请求；
6. 选择这条本地事件执行测试发送；
7. 最后检查上传服务日志和远端接口响应。

所有 delivery 成功后事件目录会被删除。
