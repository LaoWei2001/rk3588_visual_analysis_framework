---
name: rk3588-channel-logic
description: >-
  Implements or updates a per-channel C++ logic_xxx for the RK3588 vision
  system, including detection, ROI, tracking, dwell, alarm reporting and Web
  actions. Use for changes to channel logic, ChannelContext, module logic.json wiring,
  report_alarm calls, or channel action handlers. Do not use for unrelated Web
  pages, Python upload/OTA internals, or model conversion.
---

# RK3588 通道逻辑开发

> 文档角色：单通道逐帧业务规则的任务入口。上级导航：[docs 文档总入口](../../README.md) · [开发/运维知识库索引](../README.md)。

本说明以当前 `vision_analysis/src/` 为准。通道 logic 只负责逐帧业务判断、绘制和提交统一告警事件；图片/视频、投递目标、连接 Profile、字段映射和录像参数由 Web 生成的 `report_policy` 决定。

## 当前产出约定

实现一个新 `logic_xxx` 时通常修改：

1. 新建 `vision_analysis/src/logic/modules/logic_xxx/logic.cpp`，顶部包含 `logic/core/logic_common.h`，末尾用 `REGISTER_LOGIC(logic_xxx)` 自注册；函数名是 config/Web/外部 API 的唯一 logic ID；
2. 在同目录 `logic.json` 声明 `label`、模块参数 Schema、`actions` 和 `report_fields`，不手写 `name`；
3. 普通模块参数在 `logic.json.parameters` 声明，并通过 `ctx->param_*()` 读取，不增加 `ChannelConfig`/`REG_C` 中央字段；
4. 构建脚本校验并聚合所有模块，正常打包自动生成 App 根目录 `logics.json`；
5. 告知开发者在 Web 画布选择该 logic；若要投递告警，再连接一个或多个上报节点。

不要直接修改设备/App 的运行 `config.json` 作为源码功能的一部分。运行配置由 Web 保存；logic 与注册元数据才属于通用代码。

## 核心模型

- logic 签名是 `static void logic_xxx(ChannelContext *ctx)`，由框架按通道、按业务帧调用；
- `ctx->frame`、检测框、ROI 和 `draw_*` 均使用模型输入坐标系，通常是 640×640；传统 CV 确需源分辨率时可按需调用 `ctx->source_frame()`，但源坐标不能与模型坐标直接混用；
- 计时、闩锁、按 `track_id` 去重等跨帧状态放在 `ctx->state`，不能用可变 `static`；
- `ctx->timestamp_ms` 是单调时间，只适合算间隔；日历时间使用 `ctx->unix_ms`、`time_hms()` 或 `time_str()`；
- logic 运行在实时处理路径，不做 HTTP、Redis、阻塞磁盘 I/O 或视频编码。

## 告警上报模型

业务 logic 的推荐入口：

```cpp
std::string event_id = report_alarm(
    ctx,
    "person_dwell",
    "人员停留超时",
    {
        alarm_field("track_id", result.track_id),
        alarm_field("score", result.score),
        alarm_field("roi_name", roi_name),
    });
```

JSON 对象或数组使用 `alarm_json_field()`。返回非空事件 ID 表示事件已创建或与同通道、同类型的活跃事件合并；这不代表远端已经投递成功。没有有效 delivery 时返回空字符串。

正确链路是：

```text
logic -> report_alarm/alarm_report
      -> alarm_store/<event_id>/manifest.json + 图片/视频
      -> unified_upload
      -> 业务服务器或 Dify
```

连接地址和密钥保存在上传服务配置的默认连接或 `profiles` 中：仓库源码是 `service/upload/config.yaml`，安装到 App 后是 `services/upload/config.yaml`。logic 不读取连接信息，也不选择服务器/Dify；同一个事件可以由画布配置多个 delivery。

在当前模块的 `logic.json` 中声明所有需要提供给 Dify 字段映射界面的运行时字段：

```json
{
  "label": "XXX 告警",
  "parameters": {"type": "object", "additionalProperties": false, "properties": {}},
  "report_fields": [
    {"key": "track_id", "type": "number", "label": "跟踪 ID"},
    {"key": "roi_name", "type": "string", "label": "区域名"}
  ]
}
```

`report_fields[].key` 必须与 `alarm_field()` 的 key 一致，类型只使用 `string`、`number`、`boolean`、`json`。`report` 字段即使保留也只是 Web 提示元数据；是否创建投递取决于画布生成的 `report_policy.deliveries`。

完整说明见 `references/upload-and-wiring.md`，底层事件契约见 `../rk3588-src-modules/alarm.md`，Python 转发见 `../rk3588-console-ops/references/services-upload-and-ota.md`。

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
    draw_rect(ctx, hit->box, {0, 0, 255}, 3, 1.0, DrawCommand::UPLOAD);
    if (state.latched) return;
    state.latched = true;

    report_alarm(ctx, "xxx_alarm", "检测到目标", {
        alarm_field("track_id", hit->track_id),
        alarm_field("score", hit->score),
    });
}

REGISTER_LOGIC(logic_xxx);
```

`DrawCommand::UPLOAD` 表示额外加入告警图片和事件视频层。当前 Web 的“与实时播放窗口画面一致”还会复用 `DISPLAY` 层；选择原始媒体时不绘制任何叠加。不要自行 clone 帧后调用旧上传函数，图片和视频由告警模块异步生成。

## 参数与动作

普通模块参数只在该模块的 `logic.json.parameters.properties` 声明，并在同模块 C++ 中通过 `ctx->param_float/int/bool/string/json()` 读取。Schema 是默认值、类型、范围、Web 控件和热重载策略的唯一真源；完整方法见 `references/adding-config-parameter.md`。

嵌套结构如 `report_policy`、ROI、模型和 stream 有各自公共配置和热更新链路，不应复制成模块参数。通道数量、顺序或 id 变化会使整次热重载被拒绝。

自定义按钮在模块 `logic.json.actions[]` 声明，并用 `REGISTER_LOGIC_ACTION` 注册 handler。handler 只做快速状态变更；依赖当前帧的告警设置 pending 标志，由紧随其后的逐帧 logic 调用 `report_alarm()`。完整方法见 `references/custom-button-actions.md`。

## 当前源码示例

当前 `src/logic/` 的具体业务逻辑只有下列几个，示例文档必须与这张表同步：

| 需求 | 当前实现 | 文档 |
|---|---|---|
| 可删除的空白模块示例 | `modules/logic_default/logic.cpp` | `references/examples/logic_default.md` |
| 课程人员停留入门骨架 | `modules/logic_course_01/logics.cpp` | `../../二次开发课程大纲.md` |
| 统一告警、ROI、上报专用叠加 | `modules/logic_upload/logic.cpp` | `references/examples/logic_upload.md` |
| 单次事件多投递、五种绘制层、原始/叠加媒体 | `modules/logic_upload_teach/logic.cpp` | `references/examples/logic_upload_teach.md` |
| Web `+1` / `-1` action 与每通道状态 | `modules/logic_button_demo/logic.cpp` | `references/examples/logic_button_demo.md` |
| 周期截图、参数热重载、原始/叠加图片 | `modules/logic_periodic_snapshot_demo/logic.cpp` | `references/examples/logic_periodic_snapshot_demo.md` |
| 多区域 SOP、分支/循环、统一告警 | `modules/logic_path_sop/logic.cpp` | `references/examples/logic_path_sop.md` |

不要把仓库中不存在的旧 `logic_server`、`logic_dify` 等名称写成现成可选逻辑。学习统一上报和绘制层先看 `logic_upload_teach`；实现真实 ROI 报警再从 `logic_upload` 的业务闩锁和本说明的 `ctx->state` 骨架扩展。

## 接线与验证

1. `REGISTER_LOGIC(logic_xxx)` 的函数名会自动成为生成清单和运行配置中的 `channels[].logic`；源 `logic.json` 不写 `name`，模块目录建议与函数同名但不影响外部 ID；
2. 需要投递时，从 logic 节点连接上报节点；一个上报节点代表一种 delivery，可配置图片到服务器、图片到 Dify 或视频到 Dify；
3. 地址和密钥在 Web“服务配置”中管理，保存后重启 `unified_upload`；
4. 修改 C++ 后执行 `cd vision_analysis && ./build.sh <名> && sudo ./install_app.sh <名>`，再重启对应 App；
5. 修改 `config.json` 中普通字段、logic、ROI、模型、stream 或 global logic 时观察配置热重载日志；拓扑变化则重启；
6. 上报验证先看 `alarm_store/<event_id>/manifest.json`，再看 `raw.jpg`、`snapshot.jpg`、`clip.mp4` 和 `journalctl -u unified_upload`；Web“未上报告警”页也读取这份发件箱。

## 常见错误

- 类别名与 `labels.txt` 不完全一致；
- 用源分辨率坐标比较模型坐标；
- 用可变 `static` 保存每通道状态；
- 每帧提交同一事件，缺少闩锁、冷却或目标去重；
- `report_alarm()` 返回空却没有检查 delivery；
- `alarm_field()` 键与模块 `logic.json.report_fields` 不一致，导致 Dify 映射界面缺字段；
- 把连接 URL、密钥或 HTTP 请求写进 logic；
- 手改生成后的 App `logics.json`，或部署时没有同步更新二进制和生成清单。
