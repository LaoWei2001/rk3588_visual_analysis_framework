# 告警上报与画布接线参考

## 一、当前上报模型

业务 logic 只负责判断“发生了什么事件”，并调用统一入口 `report_alarm()`。保存图片还是事件视频、媒体上叠加哪些信息、投递到哪个 Profile、字段如何映射，均由 Web 画布生成的 `report_policy` 决定。

权威接口位于 `vision_analysis/src/alarm/alarm_report.h`：

```cpp
std::string report_alarm(ChannelContext *ctx,
                         const std::string &type,
                         const std::string &message,
                         std::initializer_list<AlarmField> fields);
```

返回值是事件 ID。没有有效投递配置或提交失败时返回空字符串。调用返回前会创建本地事件清单并排队媒体任务；网络投递、JPEG 编码和事件视频编码不在 logic 中执行，由后台工作线程或独立上传服务完成。

标准写法：

```cpp
std::string event_id = report_alarm(
    ctx,
    "intrusion",
    "检测到人员进入区域",
    {
        alarm_field("track_id", result.track_id),
        alarm_field("score", result.score),
        alarm_field("roi_name", roi_name),
    });
```

需要传递已经序列化的 JSON 字段时使用 `alarm_json_field()`：

```cpp
report_alarm(ctx, "inspection", "巡检异常", {
    alarm_json_field("steps", steps_json),
});
```

图片到服务器、图片到 Dify 和视频到 Dify 不需要三套业务函数。logic 只提交一次事件，Web 生成的 `report_policy.deliveries` 可以让该事件同时走多个投递分支。需要关闭事件合并、设置 `record_kind` 等高级选项时，构造 `AlarmRequest` 后调用 `alarm_report(ctx, request)`；`report_alarm()` 是它的常用简化包装。

不要在 logic 中直接选择服务器地址、Dify 密钥、图片路径或录像编码参数，也不要自行实现 HTTP/Redis 投递。这些属于上报策略和服务层职责。

## 二、画布上的上报配置如何生效

画布保存时，`web_console/frontend/src/utils/graphToConfig.ts` 将上报节点序列化到本通道配置：

- `report_policy`：投递方式、媒体种类、图片/视频叠加方式、录像前后时间、帧率、Profile 和字段映射；
- `report_parameters`：通道级上报参数；
- `event_video_pre_sec`、`event_video_post_sec`、`event_video_fps`、`event_video_overlay`：从策略派生的兼容字段。

C++ 中对应字段为：

```cpp
ctx->config->report_policy_json
ctx->config->report_parameters_json
```

logic 通常不需要读取或解析它们。`report_alarm()` 会读取当前通道配置并执行策略。

没有配置有效投递时，`report_alarm()` 返回空字符串，不会生成无目标的上报任务。因此“是否上报”由策略中是否存在有效 delivery 决定，不再使用旧的 `report_enable` 开关。

### 当前 `report_policy` 形态

一个上报节点固定产生一条 delivery；同一 logic 可以连接多个上报节点，`graphToConfig` 会把它们合并并为 delivery ID 去重。当前支持的组合只有：

| media | target | 复用的本地媒体 | 行为 |
|---|---|---|---|
| `image` | `server` | `snapshot.jpg` + `raw.jpg` | 发送固定业务 JSON，同时包含叠加图字段和原图字段 |
| `image` | `dify` | `snapshot.jpg` | 上传图片并运行 Dify workflow |
| `video` | `dify` | `clip.mp4` | 等待 MP4 完成后上传并运行 Dify workflow |

当前不支持 `video/server`。同一事件连接多个相同媒体的 delivery 时会复用已经生成的媒体文件，不会要求 logic 重复截图或重复调用上报函数。

当前 Web 生成的典型策略如下：

```json
{
  "enabled": true,
  "image_overlay": "custom",
  "video_overlay": "custom",
  "video_pre_sec": 3,
  "video_post_sec": 3,
  "video_fps": 15,
  "merge_window_sec": 5,
  "deliveries": [
    {
      "id": "delivery_report_1",
      "enabled": true,
      "media": "image",
      "target": "dify",
      "profile_id": "line_a_dify",
      "file_variable": "image",
      "file_input_mode": "single",
      "inputs": [
        {"key": "track_id", "source": "logic.track_id", "type": "number"}
      ]
    }
  ],
  "parameters": []
}
```

断开全部上报节点时，Web 写入 `{ "enabled": false, "deliveries": [] }`。C++ 以启用的有效 delivery 为准，logic 不读取顶层 `enabled`。

服务器投递当前不使用 `report_fields`/`inputs`，而是由 `EventOutboxForwarder` 生成固定 JSON，只允许画布修改 `source` 和 `eventType`。Dify 投递才根据 `inputs[].source` 从事件字段、通道参数或 logic fields 映射工作流输入。

## 三、图片、视频和叠加目标

`draw_text()`、`draw_rect()` 等函数并不立即修改 `ctx->frame`，而是构造一条 `DrawCommand` 追加到当前帧的 `ctx->draw_cmds`。显示、告警图片和事件视频随后使用同一套 `render_overlays()`，按 Target mask 选择命令并映射坐标后再真正画到各自画布。

`DrawCommand::Target` 支持精确声明自定义绘制允许进入哪些画面：

| Target | 位值含义 | 实时画面 | 图片 `custom` | 视频 `custom` |
|---|---|---:|---:|---:|
| `DISPLAY` | 实时显示层 | 是 | 是 | 是 |
| `IMAGE` | 告警图片专用层 | 否 | 是 | 否 |
| `VIDEO` | 事件视频专用层 | 否 | 否 | 是 |
| `UPLOAD` | `IMAGE | VIDEO` | 否 | 是 | 是 |
| `ALL` | `DISPLAY | IMAGE | VIDEO` | 是 | 是 | 是 |

例如只在上报媒体中叠加报警文字：

```cpp
draw_text(ctx, "报警", {20, 50}, cv::Scalar(0, 0, 255),
          1.0, 2, DrawCommand::UPLOAD);
```

实际 mask 是：实时画面取 `DISPLAY`，图片 `custom` 取 `DISPLAY|IMAGE`，视频 `custom` 取 `DISPLAY|VIDEO`。因此 `DISPLAY` 不是“永远只在屏幕显示”；当产品要求媒体与实时窗口一致时，它也会进入图片和视频。`UPLOAD` 则适合只额外写入上报媒体、不出现在实时画面的标注。

是否渲染这些命令还受到 `report_policy.image_overlay/video_overlay` 的总控制。当前 Web 提供两档：

| Web 选项 | 配置值 | 系统检测框/ROI | logic 自定义 DrawCommand |
|---|---|---:|---:|
| 当前原始帧/原始视频片段 | `none` | 不绘制 | 不绘制，Target 设置也不会绕过 `none` |
| 与实时播放窗口画面一致 | `custom` | 绘制 | 按上表的图片/视频 mask 绘制 |

目前 Web 不能逐条勾选“只要某个矩形、不要某段文字”，也没有分别暴露“仅自定义层”和“系统层+自定义层”。需要这种粒度时，应先扩展 `report_policy` 和 Web 选项，再统一修改图片与录像的模式映射，不能让业务 logic 自己解析策略并另画一套。

服务器和 Dify 消费这些媒体的规则为：

| 投递 | 收到的内容 |
|---|---|
| 图片 → 服务器 | `base64Data=snapshot.jpg`，`base64DataRaw=raw.jpg`；后者始终无叠加 |
| 图片 → Dify | 只上传 `snapshot.jpg` |
| 视频 → Dify | 上传按 `video_overlay` 生成的 `clip.mp4` |

调用顺序很重要：要进入本次告警图片的 `draw_*` 命令必须在 `report_alarm()/alarm_report()` 之前提交，因为图片任务会在上报调用期间复制当前帧的 `ctx->draw_cmds`。业务代码不应跨帧保存 `draw_cmds` 指针。

可直接运行的完整对照示例见 [logic_upload_teach.md](examples/logic_upload_teach.md)。它用五种 Target 各绘制一行文字和色块，通过 Web 按钮创建事件，便于并排检查实时画面、`snapshot.jpg`、`raw.jpg` 和 `clip.mp4`。

## 四、事件视频

当策略要求视频时，`report_alarm()` 会触发 `event_video_recorder`：

- 从环形缓存取得报警前视频；
- 继续收集报警后视频；
- 按策略指定帧率和画面模式编码 MP4；
- 编码完成后通过 `alarm_report_video_ready()` 更新事件清单。

同一通道、同一报警类型在已有事件录像窗口内再次触发时，框架可能延长现有事件，而不是重复创建录像。logic 仍需自行实现业务层限频、闩锁或按 `track_id` 去重，避免每帧重复提交事件。

推荐用 `ctx->timestamp_ms` 做间隔判断；它是单调时钟，适合计算时长。需要上报日历时间时使用 `ctx->unix_ms`、`time_hms()` 或 `time_str()`。

## 五、新增一个会上报的 logic

### 1. 实现并注册

新建 `vision_analysis/src/logic/modules/logic_xxx/logic.cpp`：

```cpp
#include "logic/core/logic_common.h"

static void logic_xxx(ChannelContext *ctx)
{
    if (!ctx || !ctx->results || !ctx->state) return;

    // 判断业务条件，并用 ctx->state 做限频/闩锁。
    report_alarm(ctx, "xxx", "发生 xxx 事件", {
        alarm_field("channel", ctx->chnId),
    });
}

REGISTER_LOGIC(logic_xxx);
```

`logic_common.h` 已包含统一告警接口，无需再包含旧的 uploader 头文件。

### 2. 在模块目录声明给 Web

新建同目录 `logic.json`，不要直接修改生成后的 App 根目录 `logics.json`：

```json
{
  "label": "XXX 报警",
  "parameters": {
    "type": "object",
    "additionalProperties": false,
    "properties": {}
  },
  "report_fields": [
    {"key": "channel", "type": "number", "label": "通道号"}
  ]
}
```

`REGISTER_LOGIC(logic_xxx)` 会把函数名自动生成为 Web 和配置使用的 logic ID，源 `logic.json` 不写 `name`。`report_fields[].key` 必须与 `alarm_field()`/`request.fields` 的 key 对齐，类型只使用 `string`、`number`、`boolean`、`json`，供 Dify 字段映射界面读取。构建脚本会校验并聚合所有模块，正常打包生成 App 根目录 `logics.json`；开发者不维护生成文件。`report` 字段即使保留也只是提示元数据，不能替代画布上的上报节点。

### 3. 画布接线

在目标通道中：

1. 视频流连接模型和逻辑节点；
2. 逻辑节点选择 `logic_xxx`；
3. 按需要添加上报配置节点；
4. 在上报配置中选择图片、视频或两者，并配置 overlay、Profile、字段映射、录像时间窗和帧率；
5. 保存后由 `graphToConfig` 写入该通道的 `report_policy`。

## 六、可调业务参数

普通业务参数在当前模块 `logic.json.parameters.properties` 声明，并通过 `ctx->param_float/int/bool/string/json()` 读取。例如 `dwell_sec` 使用 Schema `number` 和 `ctx->param_float("dwell_sec")`。不要再为模块专有参数增加 `ChannelConfig/REG_C` 中央字段，也不要手改生成的 `logics.json`。

Schema 同时提供 C++ 默认值/范围校验、Web 控件和热重载策略。完整方法见 [adding-config-parameter.md](adding-config-parameter.md)。连接地址、Profile、媒体和 `report_policy` 仍属于上报配置，不要放进模块参数。

`report_policy` 属于通用上报配置，不应为每一种 logic 重复增加一组 server/dify 地址字段。

## 七、排查清单

- `report_alarm()` 返回空：检查本通道 `report_policy` 是否包含有效 delivery；
- 有清单但没有图片或视频：检查媒体类型、overlay 和事件录像参数；
- 自定义标注只出现在显示画面：检查绘制 target 是否只有 `DISPLAY`；
- 重复产生大量事件：在 `ctx->state` 中增加闩锁、冷却或按目标去重；
- Web 看不到新 logic：检查已安装 App 根目录的 `logics.json`，不要只修改源码目录副本；
- logic 不执行：核对 `REGISTER_LOGIC()` 的函数名、生成 `logics.json` 的名称和通道 `logic` 配置。

学习完整媒体分层和多投递时使用 `vision_analysis/src/logic/modules/logic_upload_teach/logic.cpp`，对应 [logic_upload_teach.md](examples/logic_upload_teach.md)；实现真实 ROI 进入报警时再参考 `logic_upload/logic.cpp` 和 [logic_upload.md](examples/logic_upload.md)。
