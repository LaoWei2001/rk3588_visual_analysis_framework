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

全局 logic 在独立 pthread 中按 `poll_interval_ms` 轮询，可以读取多个通道的原子快照。当前源码只内置 `global_default`，新增逻辑必须显式注册；若要求 Web 可选，还要更新后端已知列表。

## 通道 logic 与全局 logic

| 维度 | 通道 logic | 全局 logic |
|---|---|---|
| 调用时机 | 本通道业务帧到达 | 独立线程周期轮询 |
| 数据 | `ctx->frame/results` | `get_channel_snapshot(ch)` |
| 状态 | 每通道 `ctx->state` | 每实例 `gctx->state` |
| 适用 | 单路检测、绘制、告警 | 多路联动、聚合、周期巡检 |
| 统一告警 | 可调用 `report_alarm(ChannelContext*)` | 当前没有接受 `GlobalContext*` 的公共入口 |

## 当前修改点

1. 在 `vision_analysis/src/logic/core/global_logic.cpp` 实现 `static void global_xxx(GlobalContext *gctx)`；
2. 在 `global_logic_register()` 中调用 `register_global_logic("global_xxx", global_xxx)`；函数定义靠后时加前置声明；
3. 在 `vision_analysis/src/logic/catalog.json` 的 `global_logics` 数组保留名称、标签和参数元数据；
4. 若要求 Web 的全局逻辑下拉可选，同步更新 `web_console/backend/routers/config_io.py::KNOWN_GLOBAL_LOGICS` 并重新部署 Web；
5. 通过 Web 或运行配置的 `global.global_logics[]` 增加实例。

未知注册名不会运行：启动时会打印 warning 并跳过该实例。当前 `GlobalLogicNode`/`GlobalLogicsPanel` 的下拉来自 `/console/info.known_global_logics`，即后端 `KNOWN_GLOBAL_LOGICS`；它不会扫描 C++ 注册表，也不直接读取 App 的 `logics.json.global_logics`。`/apps/{name}/logics` 虽会返回该元数据，但不是当前全局选择器的数据源。

## 最小骨架

```cpp
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

    if (!gctx->has_new_infer) return;
    int total = 0;
    gctx->for_each_channel([&](int ch, int) {
        ChannelSnapshot snapshot = gctx->get_channel_snapshot(ch);
        if (snapshot.frame.empty() || snapshot.result_age_ms > 500) return;
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
```

注册：

```cpp
static void global_logic_register(void)
{
    g_logic_map_count = 0;
    register_global_logic("global_default", global_default);
    register_global_logic("global_count", global_count);
}
```

`logics.json`：

```json
{
  "name": "global_count",
  "label": "跨通道人数统计",
  "params": []
}
```

运行配置位于 `global` 对象内：

```json
{
  "global": {
    "global_logics": [
      {
        "enable": true,
        "logic": "global_count",
        "channels": [0, 1, 2],
        "poll_interval_ms": 200
      }
    ]
  }
}
```

`channels: []` 表示所有活跃通道。框架可能根据受监控通道的 `max_fps` 把过慢的配置周期收紧到实时档，实际值以启动日志为准。

## 快照与状态规则

- `get_channel_snapshot()` 一次复制 frame、results、logic_state、frame_seq 和新鲜度，不能拆成多个读取；
- 快照是深拷贝，读取时不持有通道锁，也不会修改通道状态；
- `result_age_ms` 必须检查，避免使用长时间未更新的检测结果；
- `gctx->state` 每个全局实例一份，不能用可变 `static` 代替；
- `gctx->timestamp_ms` 是单调时间，只用于周期和限频；
- 全局 logic 不能调用通道的 `draw_*`，因为它没有当帧 `ChannelContext`。

## 当前告警边界

统一告警 API 只接受 `ChannelContext*`。当前 `GlobalContext` 没有 `report_alarm()`，也没有合法的 URL/媒体直传接口。因此全局规则需要产生图片或视频告警时只能选择以下设计之一：

1. 全局 logic 把联动结论写入一个明确、线程安全的共享状态，由指定通道的 channel logic 在其下一帧调用 `report_alarm()`；
2. 新增一个明确的公共 API，参数中显式指定锚点通道，并由框架安全构造告警上下文；
3. 如果只需统计或日志，不创建媒体告警。

不要伪造或强转 `ChannelContext*`，不要恢复已经删除的 uploader/Redis 链路，也不要在全局线程直接执行网络请求。实现方案 1 或 2 时必须明确状态所有权、通道下线、快照新鲜度、重复触发和锁顺序。

## 验证

1. 修改 C++ 后重新构建并安装 App；
2. 需要 Web 可选时先更新并部署 `KNOWN_GLOBAL_LOGICS`，再在“全局逻辑配置”中增加实例；也可直接检查 `global.global_logics`；
3. 配置变化会使全局逻辑线程停止并重建；
4. 启动日志应出现 `Thread started`、实际 poll 周期和 `Started N/M instance(s)`；
5. 若未启动，核对注册字符串、`logics.json` 名称、`enable` 和通道列表；
6. 单次执行时间必须显著小于实际 poll 周期，避免拖慢轮询。
