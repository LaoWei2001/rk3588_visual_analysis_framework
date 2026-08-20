# logic_dify：Dify 周期截图上报教学示例

这个模块展示一条完整、可复用的 Dify 上报链路：按 Web 画布指定的间隔截取当前通道业务帧，
把画布提示词、图片、系统字段和自定义动态变量一起发送到 Dify 工作流。

```text
logic_dify 判断周期到达
  -> EventRequest.fields 写入提示词和业务变量
  -> report_event() 创建本地事件并请求 annotated_image
  -> upload 服务等待 annotated.jpg ready
  -> POST /v1/files/upload
  -> 契约把文件 ID 和 fields.* 组装为 Dify inputs
  -> POST /v1/workflows/run
```

logic 不直接访问 Dify。Dify 地址、API Key、文件上传、字段映射、重试都属于统一上传层，
因此这个模块可以同时接 Dify、HTTP，或者连接多个上报节点，而不需要复制业务逻辑。

## 一、Dify 工作流开始节点

配套契约是：

```text
src/logic/modules/logic_dify/report_templates/dify_periodic_snapshot.json
```

Dify 工作流“开始”节点按下面的名字创建输入变量。变量名必须与契约 `target` 完全一致：

| Dify 变量名 | 建议类型 | 来源 |
|---|---|---|
| `image` | 文件列表（仅图片，最多 1 个） | 当前通道带叠加截图；边缘侧每次放入 1 张 |
| `prompt` | 段落/长文本 | 画布“Dify 提示词” |
| `channel_id` | 数字 | 系统通道 ID |
| `capture_time` | 文本 | 系统抓拍时间 |
| `scene_name` | 文本 | 画布“场景名称” |
| `custom_text` | 文本，可选 | 画布“自定义文本” |
| `report_interval_sec` | 数字 | 当前上报间隔 |
| `report_sequence` | 数字 | 当前通道成功建档序号 |
| `object_count` | 数字 | 当前帧完整目标数量 |
| `has_objects` | 布尔/复选框 | 当前帧是否有目标 |
| `detections_truncated` | 布尔/复选框 | `detections_json` 是否因长度限制被截断 |
| `detections_json` | 段落/长文本 | 检测结果 JSON 数组字符串 |
| `custom_payload_json` | 段落/长文本 | 画布自定义 JSON 对象字符串 |

如果你的 Dify 工作流只需要部分变量，可以在 Web 契约编辑器删除不需要的 mapping；不需要改
`logic.cpp`。Dify 中如果不方便使用布尔输入，也可以在契约里把相应 mapping 的 `type` 改成
`string`，或直接删除。

## 二、Web 画布配置

1. 在当前程序画布的“应用集成”中创建 Dify 投递连接，填写基础地址、工作流 App API Key 和超时；
2. 通道选择 `logic_dify`；
3. 在 logic 参数中设置“截图上报间隔”和“Dify 提示词”；提示词是多行文本框；
4. 根据业务填写场景名称、自定义文本和自定义 JSON；
5. 从 logic 节点连接“上报配置”节点；
6. 选择模块随附的 Dify 周期截图接口模板，再选择兼容的 Dify 投递连接；
7. 接收事件类型选择 `dify_periodic_snapshot`，或保持“全部事件”；
8. 保存画布。契约的 `media=["annotated_image"]` 会让 C++ 在每次事件创建时生成截图；
9. 保存后画布会绑定契约 revision；投递服务会自动重新加载连接和契约。

默认等待一个完整间隔后第一次上报。联调时可开启“首帧立即上报”。视频通道断流期间没有业务帧，
模块不会生成空图片，也不会追补断流期间错过的周期；恢复后从当前有效帧继续。

## 三、提示词为什么放在 logic 参数中

提示词不是 Dify 工作流里的固定文本，也不是契约中的 `constant`。它的路径是：

```text
Web logic 参数 prompt
  -> ctx->param_string("prompt")
  -> EventRequest.fields.prompt
  -> 契约 fields.prompt -> Dify inputs.prompt
```

这样同一套 `logic_dify` 和同一份接口契约可以复用于多个通道，每个通道拥有自己的提示词。
修改提示词采用 `preserve_state` 热更新，不会重置周期计时状态；下一次事件使用新提示词。

固定且由接口维护的文本仍可放在契约中：

```json
{
  "source": "constant",
  "target": "固定变量名",
  "value": "固定内容",
  "type": "string",
  "required": true
}
```

## 四、自定义变量的四种写法

### 1. 字符串

```cpp
request.fields.set_string("scene_name", ctx->param_string("scene_name"));
```

或：

```cpp
event_field("scene_name", ctx->param_string("scene_name"))
```

### 2. 数字

```cpp
request.fields.set_number("object_count", object_count);
```

### 3. 布尔值

```cpp
request.fields.set_bool("has_objects", object_count > 0);
```

### 4. JSON 对象或数组

```cpp
request.fields.set_json("detections", detections_json);
```

JSON 字符串必须是合法 JSON。`logic.cpp` 中的 `detections_to_json()` 演示了如何用 cJSON 安全生成
检测数组。配套契约使用 `json_string`，把对象序列化为 Dify 长文本输入，再由 Dify 代码节点解析。

每增加一个算法变量，需要同时完成两件事：

1. 在 `logic.cpp` 的 `EventRequest.fields` 写入真实值；
2. 在 `logic.json.report_fields` 声明相同 key 和类型。

然后在契约中增加：

```json
{
  "source": "fields.your_key",
  "target": "Dify开始节点变量名",
  "required": true
}
```

只修改 Dify 输入变量名时，只改契约 `target`，不改 C++。

## 五、图片类型和叠加内容

当前契约使用：

```json
{
  "source": "media.annotated_image",
  "target": "image",
  "transform": "file",
  "file_mode": "list"
}
```

- `annotated_image`：带叠加截图；具体是否显示检测框/自定义绘制由上报节点的图片叠加策略决定；
- `raw_image`：未叠加的模型输入帧；
- Dify 文件必须使用 `transform=file`，不能用 HTTP JSON 的 Base64 映射；
- 最新 Dify Workflow API 的文件型 inputs 使用文件对象数组，因此示例契约使用 `file_mode=list`；
  边缘侧每次只上传一张图，所以 `image` 数组中始终只有一个元素；
- 如果把契约媒体改成 `raw_image`，还必须在 Web 重新选择模板并保存画布，让 C++ 获得新媒体快照。

## 六、周期和可靠性语义

- 周期使用 `ctx->timestamp_ms` 单调时钟，不使用系统日历时间计算间隔；
- 每次事件设置 `EventMergeMode::NEVER`，相邻周期不会被事件合并窗口折叠；
- 本地事件创建失败时不会逐帧重建，下一次正常周期再尝试；已经进入发件箱的远端投递失败则由
  上传服务持续重试同一事件，不需要 logic 再创建一份；
- `report.accepted()` 只表示事件已经进入本地发件箱，不代表 Dify 已收到；
- 网络重试由上传服务执行，logic 线程不会被 Dify 网络请求阻塞；
- `max_detections` 只限制 JSON 明细数量，`object_count` 始终报告完整数量。

## 七、二次开发时应保持的边界

业务 logic 只计算“何时上报、上报哪些业务值”。不要在新 logic 中加入：

- Dify URL、API Key 或请求 Header；
- `requests`、curl、HTTP 客户端；
- JPG/MP4 编码和文件上传；
- Base64 编码；
- Dify 远端变量名。

地址和认证属于投递连接；远端字段名和文件类型属于接口契约；网络交互属于 adapter。保持这个边界，
同一个视觉算法才能在不改 C++ 的前提下复用到不同客户、不同 Dify 工作流和普通 HTTP 服务。
