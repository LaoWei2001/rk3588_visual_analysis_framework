---
name: rk3588-global-logic
description: >-
  Implements or updates a cross-channel global_xxx(GlobalContext*) strategy for
  the RK3588 vision system. Use when a rule aggregates multiple channels or
  periodically inspects channel snapshots. For a single channel's per-frame
  detection/alarm use rk3588-channel-logic; for Web/deployment/services use
  rk3588-console-ops.
---

# RK3588 全局逻辑开发

> 文档角色：跨通道聚合和独立周期轮询规则的任务入口。上级导航：[docs 文档总入口](../../README.md) · [开发/运维知识库索引](../README.md)。

全局 logic 在独立 pthread 中按 `poll_interval_ms` 轮询，可以读取多个通道的原子快照。它和
通道 logic 一样采用“模块目录 + 注册宏 + logic.json”机制；新增或删除模块不修改核心分发表、
共享 catalog 或 Web 硬编码列表。

## 通道 logic 与全局 logic

| 维度 | 通道 logic | 全局 logic |
|---|---|---|
| 调用时机 | 本通道业务帧到达 | 独立线程周期轮询 |
| 数据 | `ctx->frame/results` | `get_channel_snapshot(ch)` |
| 状态 | 每通道 `ctx->state` | 每实例 `gctx->state` |
| 参数 | `ctx->param_*()` | `gctx->param_*()` |
| 适用 | 单路检测、绘制、告警 | 多路联动、聚合、周期巡检 |
| 统一告警 | 可调用 `report_event(ChannelContext*)` | 当前没有接受 `GlobalContext*` 的公共入口 |

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
    if (!gctx || !gctx->state || !gctx->has_new_infer) return;
    if (!*gctx->state) *gctx->state = std::make_shared<GlobalCountState>();
    auto &state = *std::static_pointer_cast<GlobalCountState>(*gctx->state);

    const int max_age_ms = static_cast<int>(gctx->param_int("max_age_ms"));
    int total = 0;
    gctx->for_each_channel([&](int ch, int) {
        ChannelSnapshot snapshot = gctx->get_channel_snapshot(ch);
        if (snapshot.frame.empty() || snapshot.result_age_ms > max_age_ms) return;
        for (const auto &result : snapshot.results)
            if (result.label == "person") ++total;
    });
    state.total = total;

    if (gctx->timestamp_ms - state.last_log_ms >= 5000)
    {
        state.last_log_ms = gctx->timestamp_ms;
        printf("[global_count] total=%d\n", state.total);
    }
}

REGISTER_GLOBAL_LOGIC(global_count);
```

`logic.json`：

```json
{
  "label": "跨通道人数统计",
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

不要在 `logic.json` 手写 `name` 或顶层 `params`。构建器从
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
        "poll_interval_ms": 200,
        "logic_parameters": {
          "max_age_ms": 500
        }
      }
    ]
  }
}
```

`channels: []` 表示所有活跃通道。框架可能根据受监控通道的 `max_fps` 把过慢的配置周期
收紧到实时档，实际值以启动日志为准。全局配置发生变化时会停止并重建全部全局 logic
实例；参数 Schema 的 `x-hot-reload` 元数据仍用于清单表达，但全局线程当前统一重建。

## 快照与状态规则

- `get_channel_snapshot()` 一次复制 frame、results、logic_state、frame_seq 和新鲜度，不能拆成多个读取；
- 快照是深拷贝，读取时不持有通道锁，也不会修改通道状态；
- `result_age_ms` 必须检查，避免使用长时间未更新的检测结果；
- `gctx->state` 每个全局实例一份，不能用可变 `static` 代替；
- `gctx->timestamp_ms` 是单调时间，只用于周期和限频；
- 全局 logic 不能调用通道的 `draw_*`，因为它没有当帧 `ChannelContext`。

## 当前告警边界

统一告警 API 只接受 `ChannelContext*`。当前 `GlobalContext` 没有 `report_event()`，也没有合法
的 URL/媒体直传接口。因此全局规则需要产生图片或视频告警时应选择：

1. 把联动结论写入明确、线程安全的共享状态，由指定通道 logic 在下一帧提交事件；
2. 新增明确的公共 API，显式指定锚点通道并由框架安全构造告警上下文；
3. 只做统计或日志时不创建媒体告警。

不要伪造或强转 `ChannelContext*`，不要在全局线程直接执行网络请求。

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
