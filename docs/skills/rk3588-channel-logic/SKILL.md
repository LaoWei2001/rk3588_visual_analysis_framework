---
name: rk3588-channel-logic
description: >-
  Implements or updates a per-channel C++ logic_xxx for the RK3588 vision
  system, including detection, ROI, tracking, dwell, alarm reporting and Web
  actions. Use for changes to channel logic, ChannelContext, module logic.json wiring,
  report_event calls, or channel action handlers. Do not use for unrelated Web
  pages, Python upload/OTA internals, or model conversion.
---

# RK3588 通道逻辑开发

> 文档角色：单通道逐帧业务规则的任务入口。上级导航：[docs 文档总入口](../../README.md) · [开发/运维知识库索引](../README.md)。
>
> 第一次开发完整报警、图片/视频、HTTP 或 Dify 链路时，先完整阅读
> [报警事件与上报开发指南](../../报警事件与上报开发指南.md)。

本说明以当前 `vision_analysis/src/` 为准。通道 logic 只负责逐帧业务判断、绘制和提交标准事件；
接口契约决定图片/视频、固定值、字段映射和成功条件，连接 Profile 决定地址与密钥，Web 负责选择
二者并生成 `report_policy`。

## 当前产出约定

实现一个新 `logic_xxx` 时通常修改：

1. 新建 `vision_analysis/src/logic/modules/logic_xxx/logic.cpp`，顶部包含 `logic/core/logic_common.h`，末尾用 `REGISTER_LOGIC(logic_xxx)` 自注册；函数名是 config/Web/外部 API 的唯一 logic ID；
2. 在同目录 `logic.json` 声明 `label`、模块参数 Schema、`event_types`、`actions` 和
   `report_fields`，不手写 `name`；不产生事件的模块也写 `"event_types": []`；
3. 普通模块参数在 `logic.json.parameters` 声明，并通过 `ctx->param_*()` 读取，不增加 `ChannelConfig`/`REG_C` 中央字段；
4. 构建脚本校验并聚合所有模块，正常打包自动生成 App 根目录 `logics.json`；
5. 告知开发者在 Web 画布选择该 logic；若要投递告警，再连接一个或多个“上报配置”节点。

不要直接修改设备/App 的运行 `config.json` 作为源码功能的一部分。运行配置由 Web 保存；logic 与注册元数据才属于通用代码。

## 新开发者最短路径

第一次开发不要先修改框架中央文件，按下面的最小闭环完成一个新模块：

1. 阅读 `logic_default/logic.cpp + logic.json`，新建自己的 `modules/logic_xxx/`；
2. 先只实现“输入有效时画一行状态文字”，用 `REGISTER_LOGIC(logic_xxx)` 注册；
3. 业务参数全部写入同目录 `logic.json.parameters.properties`，C++ 用匹配的 `ctx->param_*()` 读取；
4. 需要跨帧计时/闩锁时再增加状态结构，放入本通道 `ctx->state`；
5. 需要按钮时增加 `actions[] + REGISTER_LOGIC_ACTION`；
6. 需要告警时增加 `event_types + report_fields + report_event()`，并按
   [告警上报与画布接线](references/upload-and-wiring.md) 完成 Profile、服务和节点接线；
7. 先运行 `python3 scripts/generate_logics_catalog.py --check`，再构建、安装并在 Web 选择新 logic；
8. 按“输入 → 画面 → 状态 → 本地事件 → 远端投递”逐层验收，不把其中一层成功当成全链路成功。

每次只增加一类能力，能显著缩短定位范围。完整课程和练习见 [二次开发课程大纲](../../二次开发课程大纲.md)。

## 核心模型

- logic 签名是 `static void logic_xxx(ChannelContext *ctx)`，由框架按通道、按业务帧调用；
- `ctx->frame`、检测框、ROI 和 `draw_*` 均使用模型输入坐标系，通常是 640×640；传统 CV 确需源分辨率时可按需调用 `ctx->source_frame()`，但源坐标不能与模型坐标直接混用；
- 计时、闩锁、按 `track_id` 去重等跨帧状态放在 `ctx->state`，不能用可变 `static`；
- `ctx->timestamp_ms` 是单调时间，只适合算间隔；日历时间使用 `ctx->unix_ms`、`time_hms()` 或 `time_str()`；
- logic 运行在实时处理路径，不做 HTTP、Redis、阻塞磁盘 I/O 或视频编码。

## 事件上报模型

业务 logic 的推荐入口：

```cpp
EventRequest request;
request.event_type = "person_dwell";
request.message = "人员停留超时";
request.fields = {
    event_field("track_id", result.track_id),
    event_field("score", result.score),
    event_field("roi_name", roi_name),
};
EventReportResult report = report_event(ctx, request);
```

JSON 对象或数组使用 `event_json_field()`。`report.accepted()` 表示事件已创建或合并，
`report.event_id` 是本地事件 ID；这不代表远端已经投递成功。失败时检查
`event_report_status_name(report.status)` 和 `report.detail`。

正确链路是：

```text
logic -> report_event
      -> event_store/<event_id>/
         event.json + media_state.json + delivery_state.json + 图片/视频
      -> unified_upload
      -> adapter -> 远端接口
```

连接地址和密钥只保存在上传服务配置的 `profiles` 中：仓库源码是 `service/upload/config.yaml`，安装到 App 后是 `services/upload/config.yaml`。logic 不读取连接信息，也不选择 adapter；同一个事件可以由画布配置多个 delivery。

在当前模块的 `logic.json` 中声明所有允许接口契约引用的运行时字段：

```json
{
  "label": "XXX 告警",
  "parameters": {"type": "object", "additionalProperties": false, "properties": {}},
  "event_types": [
    {"id": "person_dwell", "label": "人员停留超时"}
  ],
  "report_fields": [
    {"key": "track_id", "type": "number", "label": "跟踪 ID"},
    {"key": "roi_name", "type": "string", "label": "区域名"}
  ]
}
```

`event_types[].id` 必须与 `EventRequest.event_type` 完全一致。Web 只展示声明值供选择；
目录生成器会检查调用 `report_event()` 的模块不能漏掉该声明，并校验 C++ 中直接使用的事件
类型字符串。没有上报功能的模块写空数组。

`report_fields[].key` 必须与 `event_field()` 的 key 一致，类型只使用 `string`、`number`、
`boolean`、`json`。它是 C++ 动态字段的声明，不是远端接口定义；远端 target、固定值和转换写在
接口契约中。是否创建投递取决于画布生成的 `report_policy.deliveries`。

完整说明见 `references/upload-and-wiring.md`，底层事件契约见 `../rk3588-src-modules/event.md`，Python 转发见 `../rk3588-console-ops/references/services-upload-and-ota.md`。

## 最小业务骨架

```cpp
#include "logic/core/logic_common.h"

struct XxxState
{
    bool latched = false;
};

static void logic_xxx(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results) return;
    if (!*ctx->state) *ctx->state = std::make_shared<XxxState>();
    auto &state = *std::static_pointer_cast<XxxState>(*ctx->state);

    const AlgoResult *hit = nullptr;
    for (const auto &result : *ctx->results)
    {
        if (result.label != "person") continue;
        if (!roi_contains(ctx, result.box, ROI_ALL)) continue;
        hit = &result;
        break;
    }

    if (!hit)
    {
        state.latched = false;
        draw_text(ctx, "CLEAR", {20, 30}, {0, 255, 0}, 0.7, 2);
        return;
    }

    draw_text(ctx, "ALARM", {20, 30}, {0, 0, 255}, 0.7, 2);
    draw_rect(ctx, hit->box, {0, 0, 255}, 3, 1.0, DrawCommand::MEDIA);
    if (state.latched) return;
    state.latched = true;

    EventRequest request;
    request.event_type = "xxx_alarm";
    request.message = "检测到目标";
    request.fields = {
        event_field("track_id", hit->track_id),
        event_field("score", hit->score),
    };
    const EventReportResult report = report_event(ctx, request);
    if (!report.accepted())
        fprintf(stderr, "report rejected: %s (%s)\n",
                event_report_status_name(report.status), report.detail.c_str());
}

REGISTER_LOGIC(logic_xxx);
```

`DrawCommand::MEDIA` 表示额外加入告警图片和事件视频层。当前 Web 的“与实时播放窗口画面一致”还会复用 `DISPLAY` 层；选择原始媒体时不绘制任何叠加。不要自行 clone 帧后调用上传服务，图片和视频由告警模块异步生成。

## 参数与动作

普通模块参数只在该模块的 `logic.json.parameters.properties` 声明，并在同模块 C++ 中通过 `ctx->param_float/int/bool/string/json()` 读取。Schema 是默认值、类型、范围、Web 控件和热重载策略的唯一真源；完整方法见 `references/adding-config-parameter.md`。

嵌套结构如 `report_policy`、ROI、模型和 stream 有各自公共配置和热更新链路，不应复制成模块参数。通道数量、顺序或 id 变化会使整次热重载被拒绝。

自定义按钮在模块 `logic.json.actions[]` 声明，并用 `REGISTER_LOGIC_ACTION` 注册 handler。handler 只做快速状态变更；依赖当前帧的告警设置 pending 标志，由紧随其后的逐帧 logic 调用 `report_event()`。完整方法见 `references/custom-button-actions.md`。

## 当前源码示例

不要靠文档中的固定数量猜测模块清单。先运行：

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
./vision_analysis --list-logics   # 已有当前架构二进制时
```

2026-07-30 当前工作区中，适合作为开发参考的模块如下：

| 需求 | 当前实现 | 文档 |
|---|---|---|
| 可删除的空白模块示例 | `modules/logic_default/logic.cpp` | `references/examples/logic_default.md` |
| 基础绘制、参数、结果、ROI、状态和动作课程 | `modules/logic_course_01` ～ `logic_course_07` | `../../二次开发课程大纲.md`；按钮另见 `references/examples/logic_course_07.md` |
| Web 按钮触发图片/视频事件 | `modules/logic_upload_teach/logic.cpp` | `references/examples/logic_upload_teach.md` |
| 周期截图、参数热重载、原始/叠加图片 | `modules/logic_periodic_snapshot_demo/logic.cpp` | `references/examples/logic_periodic_snapshot_demo.md` |
| 同步传统 CV 取得原始帧并落盘 | `modules/logic_save_frame_pair/logic.cpp` | 直接阅读源码和 `references/channelcontext-api.md` |
| 多区域 SOP、分支/循环、统一告警 | `modules/logic_path_sop/logic.cpp` | `references/examples/logic_path_sop.md` |
| ROI 进入、闩锁和上报字段组合 | 新模块可采用的代码模式 | `references/examples/roi-alarm-pattern.md` |

`logic_course_08` ～ `logic_course_10` 当前仍是课程开发中的半成品/任务骨架，不能作为已经完成的上报实现照搬。可用模块名称必须来自当前生成的 `logics.json`，不要根据历史文档猜测。

## 接线与验证

1. `REGISTER_LOGIC(logic_xxx)` 的函数名会自动成为生成清单和运行配置中的 `channels[].logic`；源 `logic.json` 不写 `name`，模块目录建议与函数同名但不影响外部 ID；
2. 需要投递时，从 logic 节点连接“上报配置”节点；一个节点代表一条 delivery，只选择连接 Profile 和接口契约，媒体与 source→target mapping 由契约自动带出；
3. 地址和密钥在 Web“服务配置”中管理，保存后重启 `unified_upload`；
4. 修改 C++ 后执行 `cd vision_analysis && ./build.sh <名> && sudo ./install_app.sh <名>`，再重启对应 App；
5. 修改 `config.json` 中普通字段、logic、ROI、模型、stream 或 global logic 时观察配置热重载日志；拓扑变化则重启；
6. 上报验证先看 `event_store/<event_id>/event.json`、`media_state.json` 和 `delivery_state.json`，再看 `raw.jpg`、`annotated.jpg`、`clip.mp4` 和 `journalctl -u unified_upload`；Web“待上报记录”页也读取这份发件箱。所有 delivery 成功后事件目录会被移除，成功历史需从远端回执或服务日志确认。

## 常见错误

- 类别名与 `labels.txt` 不完全一致；
- 用源分辨率坐标比较模型坐标；
- 用可变 `static` 保存每通道状态；
- 每帧提交同一事件，缺少闩锁、冷却或目标去重；
- 忽略 `EventReportResult`；必须区分 `DISABLED`、`NO_DELIVERY`、`STORAGE_ERROR`、
  `WORKER_UNAVAILABLE` 和 `CREATED_MEDIA_FAILED`；
- `event_field()` 键与模块 `logic.json.report_fields` 不一致，导致接口契约引用了未声明的动态字段；
- 把连接 URL、密钥或 HTTP 请求写进 logic；
- 手改生成后的 App `logics.json`，或部署时没有同步更新二进制和生成清单。
