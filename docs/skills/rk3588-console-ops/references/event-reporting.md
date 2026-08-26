# 事件与上报开发

当前链路严格分三层：业务 C++ 创建事件，事件模块异步持久化/生成媒体，Python 服务按版本化接口
契约投递。不要让 logic 直接联网。

```text
logic → report_event()
      → event_store/<event_id>/{event.json,media_state.json,delivery_state.json,媒体}
      → unified_upload → contract revision → adapter → 远端
```

## 目录

- [C++ 事件接口](#c-事件接口)
- [Logic manifest 契约](#logic-manifest-契约)
- [有效 delivery 与 policy](#有效-delivery-与-policy)
- [图片和视频](#图片和视频)
- [本地 schema v3](#本地-schema-v3)
- [连接与版本化契约](#连接与版本化契约)
- [当前适配器](#当前适配器)
- [投递状态与重试](#投递状态与重试)
- [分层验收](#分层验收)

## C++ 事件接口

```cpp
// result 和 detail_json 代表本次业务已经取得的检测结果与 JSON 字符串。
EventRequest request;
request.event_type = "person_alarm";
request.message = "检测到人员";
request.fields = {
    event_field("track_id", result.track_id),
    event_field("score", result.score),
    event_json_field("detail", detail_json),
};

const EventReportResult report = report_event(ctx, request);
if (!report.accepted())
    fprintf(stderr, "report rejected: %s (%s)\n",
            event_report_status_name(report.status), report.detail.c_str());
```

`EventRequest` 当前字段：`event_type`、`message`、`fields`、`merge_mode` 和
`source_channel_id`。通道 logic 不需设置来源；全局 logic 可设置动态来源。

结果状态：

| 状态 | `accepted()` | 含义 |
|---|---:|---|
| `CREATED` | 是 | 已排入本地持久化队列 |
| `MERGED` | 是 | 合并更新已排入本地持久化队列 |
| `CREATED_MEDIA_FAILED` | 是 | 事件仍排队，但所需媒体已立即判定失败 |
| `DISABLED` | 否 | policy 明确禁用 |
| `INVALID_REQUEST` | 否 | 上下文、事件类型或媒体来源无效 |
| `NO_DELIVERY` | 否 | 没有有效且匹配本事件的 delivery |
| `WORKER_UNAVAILABLE` | 否 | 本地持久化/图片 worker 无法启动 |
| `STORAGE_ERROR` | 否 | 状态序列化失败 |

`accepted()` 只表示请求进入内存中的本地持久化链路；它不保证事件文件已经写完、媒体已生成或远端
已接收。

## Logic manifest 契约

- `event_types[].id` 必须与 C++ `request.event_type` 一致；
- `report_fields[].key/type` 必须与 `event_field()`/`event_json_field()` 一致；
- 模块模板路径写入 `report_templates[]`，模板 `owner_logic` 必须是注册 ID；
- `generate_logics_catalog.py --check` 会静态核对字面量事件和字段。

## 有效 delivery 与 policy

C++ 只为满足以下条件的 delivery 创建状态：

- policy 不是 `enabled: false`；
- delivery 不是 `enabled: false`；
- `connection_id`、`contract_id`、`contract_revision` 都是非空字符串；
- `media` 是数组，可以为空表示纯数据；
- `when.event_types` 缺失/空，或包含当前事件类型。

Web 画布负责生成这些字段并绑定当前契约 revision。不要在 C++ logic 中选择 adapter、URL、密钥、
远端字段名或媒体文件路径。

`merge_mode` 默认 `EventMergeMode::NEVER`。只有 C++ 显式设为 `POLICY` 且 policy 的
`merge_window_sec > 0` 才合并；窗口会限制到 0–60 秒，缺省为 5 秒。合并 key 是
`source channel id + event_type`。仅在 Web 把窗口填为非零不会自动改变默认 NEVER。

## 图片和视频

契约媒体可包含：`raw_image`、`annotated_image`、`video`。

- 通道图片来自事件帧的模型尺寸图；标注模式会复用系统标注，以及目标为 DISPLAY 或 IMAGE 的
  自定义绘制。
- `image_overlay: "none"` 不叠加；当前 Web 的“与实时画面一致”写为 `custom`。
- 视频由后台 recorder 使用预录环形缓冲和 `video_pre_sec/video_post_sec/video_fps` 生成。
- `video_overlay` 接受 `none/custom/all`；当前帧入口把 `custom` 和 `all` 都映射为同一个
  `EVENT_VIDEO_OVERLAY_DISPLAY` 分支，按实时显示尺寸和绘制规则生成，只有 `none` 走无叠加源帧。
  这也意味着 `custom` 与 `all` 当前没有运行差异。
- 当前 Web 上报表单可改图片叠加和视频前后时长/FPS，但没有视频叠加选择器；新节点默认
  `video_overlay: "custom"`，导入配置中的现值会往返保留。需要其他值时不能假装已有可见控件。

全局事件的来源解析顺序：`request.source_channel_id` → `media_source_channel_id` → 第一个画布连入
通道 → 应用第一个通道。有连入通道且需要图片时，图片按全局显示尺寸/宫格将所有连入通道拼接；
没有连入通道时只取解析后的来源通道。

`request.source_channel_id` 决定事件 `source.channel_id` 和图片回退来源，但事件视频始终使用全局
配置的 `media_source_channel_id`。全局启用视频时该字段必须是存在的通道 ID，以便预先建立录像
缓冲。

## 本地 schema v3

事件目录由三个状态文件分离所有权：

| 文件 | 写入者 | 内容 |
|---|---|---|
| `event.json` | C++ | `event`、`source`、`data.fields`、`policy_snapshot` |
| `media_state.json` | C++ 图片/录像模块 | 图片/视频状态与文件名 |
| `delivery_state.json` | C++ 初始化，随后 Python 独占 | deliveries、attempts、错误、重试时间 |

C++ 最后写 `event.json`，它同时是“目录创建完整并可被消费者看见”的标记。常见媒体文件名是
`raw.jpg`、`annotated.jpg`、`clip.mp4`，只有契约要求且生成成功时才存在。

直接运行二进制时，事件目录默认 `./event_store`；`EVENT_STORE_DIR` 可覆盖。Web 启动 App 时设置为
`/opt/ai_apps/.data/<App>/event_store`。默认容量上限 1 GiB、最小剩余空间 512 MiB，可分别用
`EVENT_STORE_MAX_BYTES`、`EVENT_STORE_MIN_FREE_BYTES` 覆盖。超限时会从最旧且没有正在生成/上传
工作的待处理事件开始回收，因此本地 outbox 不是无限期归档。

## 连接与版本化契约

Web 管理模式下：

- 连接：`.data/<App>/connections.yaml`；
- 程序包模板：`<App>/report_templates/*.json`；
- 用户新增契约：`.data/<App>/report_contracts/*.json`；
- immutable revision 归档：`.data/<App>/contract_revisions/<sha256>.json`。

活动契约加载时，同 ID 的程序包模板优先于 custom 文件。Web 编辑一个程序包模板时，当前后端会
直接改已安装 App 的 `report_templates` 文件，并移除同 ID 的旧 custom override；重新安装同名包会
覆盖这类包内修改。需要长期随版本交付的改动应修改源码模块模板并重新构建。新 ID 的自定义契约
位于 `.data`，可跨包替换保留。

已排队事件保存 `contract_id + contract_revision`。上传服务按归档 revision 重放，因此后来编辑当前
模板不会偷偷改变旧事件请求。

映射来源根为 `event`、`source`、`fields`、`media`，或使用 `source: "constant"`。位置支持
`body/query/form/header/file`，但 Dify 只允许 body/file。类型转换支持 string/number/boolean/json；
适配器目录当前给 HTTP 开放 `base64/data_url/json_string/file`，给 Dify 开放
`json_string/file`。

## 当前适配器

### HTTP (`http`)

连接字段：`base_url`、`timeout`（默认 15 秒）、`headers`。没有 scheme 的地址自动补 `http://`。
契约 method 仅支持 POST/PUT/PATCH，encoding 仅支持 json/form/multipart；映射可落 body、query、
form、header、file。请求自动添加 `X-Idempotency-Key: <event_id>:<delivery_id>`，除非连接/映射已经
提供该 header。成功由 `success.http_status` 及可选的 `json_path/equals` 判定。

### Dify (`dify_workflow`)

连接字段：`api_url`、`api_key`、`timeout`（默认 120 秒）。文件先传 `/v1/files/upload`，再以
`response_mode: blocking` 调 `/v1/workflows/run`。映射只允许 body/file；file 可按 single/list 注入。

## 投递状态与重试

delivery 状态包括 `pending`、`uploading`、`delivered`、`retry`、`invalid`、`failed`。

- 网络异常以及 HTTP 408/425/429/5xx 进入 retry；
- 其他 4xx 属于终止性失败；
- 媒体永久生成失败变为 failed；
- 契约 revision、连接或 mapping 无效变为 invalid；
- retry 从 10 秒指数退避，最长 300 秒，当前没有最大尝试次数；
- Web“重试”会把所有非 delivered delivery 重置为 pending；
- 同一事件全部 delivery 为 delivered 后删除整个事件目录。

## 分层验收

1. 检查 C++ 返回状态和 `event_id`。
2. 等待目录出现，分别查看三个 JSON，不把 `event.json` 单文件当成全部状态。
3. 核对必需媒体状态和真实文件。
4. 核对 delivery 使用的 connection、contract ID/revision、状态与 `last_error`。
5. 查看 `journalctl -u unified_upload.service`。
6. 用接口预览检查 URL/headers/body/files，再用一条真实本地事件做测试发送。
7. 最后以远端响应或远端业务系统为成功依据。
