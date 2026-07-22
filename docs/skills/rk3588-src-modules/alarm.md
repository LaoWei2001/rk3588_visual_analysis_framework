# `src/alarm`：告警事件与发件箱

核心文件为 `alarm_report.h/.cpp`。业务 logic 只提交事件类型、消息和运行时字段；媒体类型、投递目标、叠加方式、视频窗口及字段映射来自通道 `report_policy`。

## 公共入口

```cpp
std::string report_alarm(ChannelContext *ctx,
                         const std::string &type,
                         const std::string &message,
                         std::initializer_list<AlarmField> fields);
```

字段可用 `alarm_field()` 传字符串、布尔或数值，JSON 对象/数组必须用 `alarm_json_field()`。返回非空值表示已创建或合并事件；调用非阻塞，不代表远端已发送成功。

```cpp
const std::string event_id = report_alarm(
    ctx, "person_enter", "检测到人员进入",
    {alarm_field("label", result.label),
     alarm_field("score", result.score)});
```

可上报字段元数据还需在 `src/logic/modules/logic_xxx/logic.json` 的 `report_fields` 中声明，`key` 必须与 C++ 一致，类型支持 `string`、`number`、`boolean`、`json`；打包时会自动聚合到 Web 清单。

## 内部行为

- 解析 `ctx->config->report_policy_json`；没有有效 delivery 时不建事件。
- 为事件创建 `alarm_store/<event_id>/manifest.json`，默认目录可由 `ALARM_STORE_DIR` 覆盖；C++、上传服务和 Web 记录页使用同一个变量名。
- 图片由单独 worker 异步生成，同时保存 `raw.jpg` 和 `snapshot.jpg`；`image_overlay=none` 时二者相同，否则 snapshot 调用统一 `render_overlays()`。
- 视频 delivery 触发 recorder，并由 `alarm_report_video_ready()` 在 MP4 完成后回写清单。
- 同通道、同报警类型的活跃事件会合并触发并延长录像后窗，避免连续告警制造大量事件。
- 发件箱容量上限为 1 GiB，并预留至少 512 MiB 可用空间；清理依据事件目录/清单状态执行。

当前固定路由是图片到服务器、图片到 Dify、视频到 Dify。实际发送由外部服务完成，见 [uploader.md](uploader.md)。

一个 `alarm_report()` 调用只建立一个事件，但 `report_policy.deliveries` 可以同时包含上述多个路由；图片 delivery 复用同一组 `snapshot.jpg/raw.jpg`，视频 delivery 复用同一个 `clip.mp4`。业务 logic 不按目标重复调用。可运行演示见 `../rk3588-channel-logic/references/examples/logic_upload_teach.md`。

## manifest 契约

新事件的清单包含以下核心字段；图片 worker、录像模块和上传服务会继续原子更新同一文件：

```json
{
  "schema_version": 1,
  "event_id": "...",
  "channel_id": 0,
  "alarm_type": "person_enter",
  "message": "检测到人员进入",
  "trigger_unix_ms": 0,
  "last_trigger_unix_ms": 0,
  "trigger_count": 1,
  "state": "collecting",
  "fields": {"label": "person", "score": 0.91},
  "channel_parameters": {},
  "media": {"snapshot": "", "raw": "", "video": ""},
  "media_requested": {"image": true, "video": false},
  "deliveries": [
    {"id": "delivery_1", "media": "image", "target": "server",
     "status": "pending", "attempts": 0, "last_error": ""}
  ],
  "policy_snapshot": {}
}
```

同通道、同 `alarm_type` 在 `merge_window_sec` 内再次触发时返回原事件 ID，并更新 `trigger_count`、最后触发时间、最新 fields 和 `merged_triggers`。业务代码仍应做闩锁或限频，避免无意义的高频清单更新。

## 叠加目标

logic 的绘制指令必须选对目标。当前叠加图片的 target mask 是 `DISPLAY|IMAGE`，所以实时层和图片专用命令都会进入 `snapshot.jpg`；叠加视频使用 `DISPLAY|VIDEO`。`UPLOAD=IMAGE|VIDEO`，`ALL` 包含三者。媒体选择原始模式时任何叠加都不会绘制。换言之，`DISPLAY` 并非“绝不进入告警媒体”，因为产品的叠加模式明确要求复用实时画面层。

图片任务在 `alarm_report()` 内复制当帧 `ctx->draw_cmds`，所以需要进入当前截图的命令必须先调用 `draw_*` 再上报。`draw_cmds` 是命令队列而非已绘制像素；不要跨帧保存它。

## 退出约束

先调用 `event_video_recorder_deinit()` 完成 MP4 和清单回写，再调用 `alarm_report_deinit()` 停图片 worker。不要反转顺序。
