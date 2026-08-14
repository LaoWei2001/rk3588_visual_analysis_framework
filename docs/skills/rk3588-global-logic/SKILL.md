---
name: rk3588-global-logic
description: >-
  Implements or updates a cross-channel global_xxx(GlobalContext*) strategy for
  the RK3588 vision system. Use when a rule aggregates multiple channels or
  consumes per-tick ChannelLogicSnapshot batches. For a single channel's per-frame
  detection/alarm use rk3588-channel-logic; for Web/deployment/services use
  rk3588-console-ops.
---

# RK3588 全局逻辑开发

> 文档角色：跨通道聚合和独立周期轮询规则的任务入口。上级导航：[docs 文档总入口](../../README.md) · [开发/运维知识库索引](../README.md)。

全局 logic 在独立 pthread 中按实际调度周期轮询。每个 tick 先固定采样所有输入通道的
`ChannelLogicSnapshot`，算法读取变量时不复制图像，也不会在一次调用中跨版本。它和
通道 logic 一样采用“模块目录 + 注册宏 + logic.json”机制；新增或删除模块不修改核心分发表、
共享 catalog 或 Web 硬编码列表。

## 通道 logic 与全局 logic

| 维度 | 通道 logic | 全局 logic |
|---|---|---|
| 调用时机 | 本通道业务帧到达 | 独立线程周期轮询 |
| 数据 | `ctx->frame/results` | 本 tick 固定的 `ChannelLogicSnapshot` 批次 |
| 状态 | 每通道 `ctx->state` | 每实例 `gctx->state` |
| 参数 | `ctx->param_*()` | `gctx->param_*()` |
| 适用 | 单路检测、绘制、告警 | 多路联动、聚合、周期巡检 |
| 统一告警 | `report_event(ctx, request)` | 同样调用 `report_event(gctx, request)`，共用 EventRequest、策略、媒体和发件箱 |

## 模块结构

```text
vision_analysis/src/logic/global_modules/global_count/
├── logic.cpp
└── logic.json
```

`logic.cpp`：

```cpp
#include "logic/core/global_logic.h"

struct GlobalCountState
{
    int total = 0;
    uint64_t last_log_ms = 0;
};

static void global_count(GlobalContext *gctx)
{
    if (!gctx || !gctx->state) return;
    if (!*gctx->state) *gctx->state = std::make_shared<GlobalCountState>();
    auto &state = *std::static_pointer_cast<GlobalCountState>(*gctx->state);

    const int max_age_ms = static_cast<int>(gctx->param_int("max_age_ms"));
    int total = 0;
    gctx->for_each_channel([&](const ChannelLogicSnapshot &channel, int) {
        if (!channel.has_publication || channel.online_state != CH_ONLINE ||
            channel.publication_age_ms > max_age_ms) return;
        int64_t person_count = 0;
        if (channel.outputs.try_get_int("person_count", &person_count))
            total += static_cast<int>(person_count);
    });
    state.total = total;

    if (gctx->steady_ms - state.last_log_ms >= 5000)
    {
        state.last_log_ms = gctx->steady_ms;
        printf("[global_count] total=%d\n", state.total);
    }
}

REGISTER_GLOBAL_LOGIC(global_count);
```

`logic.json`：

```json
{
  "label": "跨通道人数统计",
  "event_types": [],
  "report_fields": [],
  "business_fields": [],
  "parameters": {
    "type": "object",
    "additionalProperties": false,
    "properties": {
      "max_age_ms": {
        "type": "integer",
        "title": "结果新鲜度",
        "minimum": 10,
        "maximum": 10000,
        "default": 500,
        "x-unit": "毫秒"
      }
    }
  }
}
```

不要在 `logic.json` 手写 `name` 或顶层 `params`。不产生事件时也必须显式写
`"event_types": []`；`report_fields`/`business_fields` 没有声明时写空数组。构建器从
`REGISTER_GLOBAL_LOGIC(global_count)` 生成唯一 ID，并从 JSON Schema 生成 Web 参数表。

运行配置位于 `global` 对象内：

```json
{
  "global": {
    "global_logics": [
      {
        "enable": true,
        "logic": "global_count",
        "channels": [0, 1, 2],
        "channels_explicit": true,
        "poll_interval_ms": 200,
        "logic_parameters": {
          "max_age_ms": 500
        }
      }
    ]
  }
}
```

画布保存的 `channels_explicit: true` 表示输入完全由连线决定，此时 `channels: []` 就是没有输入；
仅为兼容旧手写配置，缺少 `channels_explicit` 时 `channels: []` 仍表示所有活跃通道。框架可能根据受监控通道的 `max_fps` 把过慢的配置周期
收紧到实时档，实际值以启动日志为准。全局配置发生变化时会停止并重建全部全局 logic
实例；参数 Schema 的 `x-hot-reload` 元数据仍用于清单表达，但全局线程当前统一重建。

## 输入批次与状态规则

- `channel_snapshots` 是本 tick 固定的轻量批次，包含 outputs、同代参数、版本、时间、在线状态和性能元信息；
- `has_updates()` 只在至少一个输入通道真正提交了新版本时为真，推理提交动作本身不会触发；
- 若算法包含超时、断流复位或周期巡检，不要在函数开头无条件用 `has_updates()` 返回；即使没有新版本，
  `publication_age_ms` 仍在增长。只做新版本增量计算时才用它跳过 tick；
- `for_each_updated_channel()` 可只遍历本 tick 发生变化的通道；`ChannelUpdate::missed_revisions > 0`
  表示轮询期间出现过中间版本，瞬时 outputs 只保留最新状态；
- `publication_age_ms` 必须检查，避免使用长时间未更新的业务变量；
- 不同通道分别原子采样，但不保证来自同一采集时刻；同步算法比较 `frame_steady_ms` 并限定偏差；
- 只有确实需要图像、原始检测结果或绘制指令时才调用 `get_channel_frame_snapshot()`。媒体快照会深拷贝图像，
  而且可能比本 tick 批次更新；需要核对版本时比较 `frame.logic.publication_seq`；
- `gctx->state` 每个全局实例一份，不能用可变 `static` 代替；
- `gctx->steady_ms` 用于周期和限频，`unix_ms` 用于墙钟记录，`dt_ms` 是真实 tick 间隔；
- `effective_poll_interval_ms` 是框架实际采用的周期，不能假定它始终等于配置值；
- 全局 logic 不能调用通道的 `draw_*`，因为它没有当帧 `ChannelContext`。

通道 logic 对外公开的变量和产生这些变量时使用的参数位于同一份轻量快照中：

```cpp
const ChannelLogicSnapshot *channel = gctx->channel(0);
if (!channel || !channel->has_publication || channel->publication_age_ms > 500)
    return;

int64_t count = 0;
bool alarming = false;
if (!channel->outputs.try_get_int("person_count", &count) ||
    !channel->outputs.try_get_bool("alarm_active", &alarming))
    return; // key 不存在或类型不匹配，不能与合法的 0/false 混为一谈

float threshold = channel->parameters.get_float("threshold");
```

全局接口不暴露通道 `logic_state`。跨模块数据只能通过 logic.json `outputs` 契约和 `publish_*()` 传递。

## 与通道逻辑统一的事件上报

全局逻辑与通道逻辑使用同一个 `EventRequest` 和 `report_event()`。上报目标、接口模板、图片、
视频、叠加方式、合并窗口及发件箱都来自画布连接的同一种“上报配置”节点：

```cpp
EventRequest request;
request.event_type = "multi_channel_alarm";
request.message = "多路联动报警";
request.fields = { event_field("total", total) };
request.source_channel_id = 1; // 事件来源/视频来自通道 1；省略则使用上报节点默认值

EventReportResult result = report_event(gctx, request);
if (!result.accepted())
    fprintf(stderr, "report rejected: %s\n", result.detail.c_str());
```

全局事件图片使用所有连入通道各自最近一次内部对齐的快照，按全局显示宽高与窗格行列顺序拼接；
不同通道的快照不保证来自同一采集时刻，要求时间同步的业务必须比较时间戳并限定允许偏差；
同一上报节点的 `image_overlay` 模式会应用到每个窗格，决定是否叠加各通道的系统标注及自定义
图形/文字，并不支持在一次全局图片中为每个通道选择不同模式。需要动态选择事件视频通道时，
应把可能使用的通道都连入该全局节点，以便提前建立预录缓冲。事件视频继续复用单通道录像器，
不在全局线程直接联网或编码。

## 画布接线

从每个参与聚合的“逻辑函数/SOP”节点底部连到“全局逻辑”节点，再从全局逻辑右侧连接已有的
“上报配置”节点。配置保存时自动从连线生成 `global.global_logics[].channels`，无需再手填通道号；
选中全局节点可查看各输入通道声明的 outputs 和参数契约。

可直接运行和照着修改的端到端范例见
[双通道变量聚合与上报示例](references/two-channel-canvas-demo.md)。它包含两个视频通道复用同一个
通道逻辑、类型化变量发布、通道参数读取、全局组合判断、动态选择媒体来源和统一事件上报。

## 验证

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
./build.sh --debug
./vision_analysis --list-global-logics
```

还应确认：

- 生成的 `logics.json.global_logics` 包含模块名称、标签和参数；
- Web 全局逻辑下拉来自当前 App 清单，参数值写入 `logic_parameters`；
- 启动日志出现 `Thread started` 和 `Started N/M instance(s)`；
- 未知注册名被跳过，不会回退到其他模块；
- 单次执行时间显著小于实际 poll 周期。
