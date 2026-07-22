# logic_upload_teach — 统一图片/视频上报教学示例

- 源码：`rk3588_yolo/src/logic/modules/logic_upload_teach/logic.cpp`
- 模块清单：同目录 `logic.json`
- 事件类型：`upload_teach_demo`
- 触发方式：Web 动作“立即生成教学事件”

这个模块只用于学习统一上报链路，不依赖模型结果或 ROI，也不实现复杂报警条件。它重点演示：

1. 通道 logic 只调用一次 `alarm_report()`；
2. Web 可以让同一事件同时投递服务器图片、Dify 图片和 Dify 视频；
3. `DrawCommand::Target` 如何决定自定义图形/文字进入实时画面、图片或视频；
4. Web 的原始/叠加选择如何覆盖 C++ 绘制目标；
5. 服务器固定 JSON 与 Dify 可映射业务字段的区别。

## 一、运行前接线

在 Web 的目标通道中：

1. 按正常通道拓扑连接可持续出帧的视频源、模型和 logic；本教学逻辑不读取检测结果，模型只用于保持现有通道接线结构；
2. logic 选择“统一上报教学示例”；
3. 从 logic 节点按需要连接以下一个或多个上报节点：

| 节点配置 | 作用 |
|---|---|
| 图片 → 服务器 | 向业务服务器发送固定 JSON、叠加图和原图 |
| 图片 → Dify | 上传 `snapshot.jpg` 并运行工作流 |
| 视频 → Dify | 生成并上传 `clip.mp4`，可配置前后窗和 FPS |

4. 为图片、视频选择“与实时播放窗口画面一致”，保存画布配置并确保 App 正在运行；
5. 回到“程序管理”，打开该 App 的“实时画面”；
6. 在视频下方“通道控制”区域找到目标通道，点击“立即生成教学事件”。

动作按钮不在编辑画布的 logic 节点属性面板中。实时画面弹窗会从已安装 App 根目录的 `logics.json` 读取当前 logic 的 `actions[]`；如果按钮没有出现，确认使用正常打包生成的新清单、重新安装 App，并关闭后重新打开实时画面弹窗。

一次点击只调用一次 C++ 上报入口，但一个事件清单中可以有多条 delivery。各 delivery 独立上传、重试和记录状态。

## 二、五种绘制目标会看到什么

逻辑持续生成五行不同颜色的文字和色块：

```cpp
draw_target_row(ctx, "DISPLAY: live + custom media", ..., DrawCommand::DISPLAY);
draw_target_row(ctx, "IMAGE: custom image only",    ..., DrawCommand::IMAGE);
draw_target_row(ctx, "VIDEO: custom video only",    ..., DrawCommand::VIDEO);
draw_target_row(ctx, "UPLOAD: custom image + video",..., DrawCommand::UPLOAD);
draw_target_row(ctx, "ALL: live + custom image + video", ..., DrawCommand::ALL);
```

在 Web 选择叠加模式 `custom` 时，预期结果如下：

| 文字/色块 Target | 实时画面 | `snapshot.jpg` | `clip.mp4` |
|---|---:|---:|---:|
| `DISPLAY` | 显示 | 显示 | 显示 |
| `IMAGE` | 不显示 | 显示 | 不显示 |
| `VIDEO` | 不显示 | 不显示 | 显示 |
| `UPLOAD` | 不显示 | 显示 | 显示 |
| `ALL` | 显示 | 显示 | 显示 |

原因是三个渲染出口使用的 Target mask 分别是：

```text
实时画面              DISPLAY
图片 custom          DISPLAY | IMAGE
视频 custom          DISPLAY | VIDEO
```

`UPLOAD` 是 `IMAGE | VIDEO`，`ALL` 是 `DISPLAY | IMAGE | VIDEO`。

## 三、切换为原始媒体

把上报节点的图片或视频叠加内容切换为“当前原始帧/原始视频片段”，保存后再次点击按钮：

- 原始图片不会出现上述任何一行，也不会出现检测框、ROI 等系统叠加；
- 原始视频同样不渲染任何自定义或系统叠加；
- C++ 不需要改变 Target，也不需要重新编译。

因此 Target 表达的是“这条命令允许进入哪些渲染层”，Web 的 `none` 则是媒体出口的总开关。`none` 优先级更高，会跳过全部绘制命令。目前 Web 只支持“全部原始”和“与实时画面一致”两档，不能逐条勾选某个矩形或某段文字。

## 四、一次 C++ 调用如何产生多种投递

教学逻辑在当前帧先完成全部 `draw_*` 调用，然后只提交一次：

```cpp
AlarmRequest request;
request.type = "upload_teach_demo";
request.message = "统一图片/视频上报教学事件";
request.merge_enabled = false;
request.fields.set_number("event_sequence", sequence);
request.fields.set_string("trigger_source", "web_action");
request.fields.set_bool("one_call_many_deliveries", true);
request.fields.set_json(
    "draw_targets",
    "[\"DISPLAY\",\"IMAGE\",\"VIDEO\",\"UPLOAD\",\"ALL\"]");

const std::string event_id = alarm_report(ctx, request);
```

这里使用完整 `AlarmRequest` 只是为了设置 `merge_enabled=false`，让每次教学按钮点击都产生独立事件。普通业务没有这个要求时可使用更短的 `report_alarm()` 包装函数。二者进入同一套告警发件箱。

Web 保存的 `report_policy.deliveries` 决定后续分支：

```text
一次 alarm_report()
  └─ 一个 alarm_store/<event_id>/manifest.json
       ├─ image/server delivery ── snapshot.jpg + raw.jpg ── 业务服务器
       ├─ image/dify delivery   ── snapshot.jpg ─────────── Dify
       └─ video/dify delivery   ── clip.mp4 ─────────────── Dify
```

当前不支持视频直接投递业务服务器。

## 五、业务字段如何上报

教学模块在 `logic.json.report_fields` 声明了：

- `event_sequence`：数字；
- `trigger_source`：字符串；
- `one_call_many_deliveries`：布尔；
- `draw_targets`：JSON 数组。

Dify 上报节点可以把这些字段映射为工作流输入。服务器图片接口使用固定业务协议，logic fields 不会自动进入服务器 JSON；服务器收到的是固定的 `source/eventType/detResult/snapTime/endTime/base64Data/base64DataRaw/invadeFlag/eventId`。

其中服务器字段：

- `base64Data` 对应 `snapshot.jpg`；
- `base64DataRaw` 对应 `raw.jpg`；
- 叠加模式下前者带信息、后者始终是原始帧；
- 原始模式下两者都不带叠加。

## 六、`draw_cmds` 在示例中的位置

示例没有直接写 `ctx->draw_cmds->push_back(...)`，但它一直在使用 `draw_cmds`。例如 `draw_text()` 内部会构造 `DrawCommand::TEXT`，再追加到 `*ctx->draw_cmds`。

```text
logic 调 draw_text/draw_rect
       └─ 追加到本帧 ctx->draw_cmds
            ├─ logic 返回后保存给实时显示
            ├─ alarm_report 调用时复制给图片任务
            └─ 录像帧入口复制给事件视频渲染
```

这是延迟渲染的命令列表，不是持久状态，也不是已经画好的图片。不要跨帧保存 `ctx->draw_cmds` 指针，也不要绕过 `draw_*` 直接操作，除非正在扩展框架本身的新绘制类型。

## 七、验证与排障

1. 实时画面只应看到 `DISPLAY`、`ALL` 两行以及教学状态文字；
2. Web 记录页检查事件是否生成，并分别查看 `snapshot.jpg` 和 `raw.jpg`；
3. 自定义图片应看到 `DISPLAY/IMAGE/UPLOAD/ALL`，不应看到 `VIDEO`；
4. 自定义视频应看到 `DISPLAY/VIDEO/UPLOAD/ALL`，不应看到 `IMAGE`；
5. 检查 `manifest.json.deliveries[]` 的 `status/attempts/last_error`；
6. 没有事件时确认至少连接并启用了一个有效上报节点；
7. 有事件但远端失败时检查 `journalctl -u unified_upload`、Profile、URL 和鉴权。

`alarm_report()` 返回非空只表示事件已进入本地发件箱，不表示所有远端 delivery 已经成功。
