---
name: rk3588-global-logic
description: >-
  Develop, review, or explain a cross-channel global C++ logic module in this
  RK3588 visual-analysis framework. Use for GlobalContext, typed channel inputs,
  event-driven scheduling, multi-channel state, update versions, global actions, media source
  selection, or global event reporting. Prefer inputs() for business data and
  verify behavior against the current global scheduler and module manifests.
---

> 独立项目：业务根目录由 `--project` 选择，模块位于该项目 `logic/`；公开 API 位于引擎 `vision_analysis/include/rkvision/`。向导隔离副本中引擎和本文档位于只读 `.engine/`。使用生成器时显式传 `--logic-root <项目>/logic`；省略参数只检查框架参考项目。


# RK3588 全局逻辑开发

本 Skill 面向跨通道组合判断和独立周期任务。当前权威源码是：

- `vision_analysis/include/rkvision/global_context.h 和 vision_analysis/src/logic/core/global_logic.cpp`；
- `vision_analysis/src/runtime/app_ctrl.h` 中的通道快照；
- `logic/global_modules/`；
- `vision_analysis/src/config/config.h/.cpp` 中的 `GlobalLogicConfig`。

每个启用的全局实例拥有一个 pthread 和一份独立 state。任一通道发布新业务快照时立即唤醒实例；
`poll_interval_ms` 仅在没有新发布时提供周期性兜底，不是固定执行周期。

## 选择通道还是全局 logic

| 问题 | 使用位置 |
|---|---|
| 当前通道逐帧检测、ROI、画框 | 通道 logic |
| 两路或多路 outputs 联动 | 全局 logic |
| 与视频帧无关的独立周期判断 | 全局 logic |
| 需要在某个通道画叠加 | 通道 logic；全局上下文没有 `draw_*` |
| 通道之间传业务值 | 上游 `publish_*()`，下游 `inputs()` |

单通道接口见 [`rk3588-channel-logic`](../rk3588-channel-logic/SKILL.md)。

## Logic-only 写入规则

通过 `vision develop` 运行时，只能写入 `logic/modules/**` 和
`logic/global_modules/**`。全局模块及其 manifest/模板必须放在所属
`global_modules/<global_logic_id>/`；需要新增上游输出时，只能修改或新增 `modules/<logic_id>/`。
公共 `logic/core`、运行配置、测试、Web、服务、文档、脚本和生成物全部只读。现有接口不足时立即停止，
不请求扩大权限。机械执行规则见
[`Logic-only 写入边界`](../rk3588-feature-wizard/references/write-boundary.md)。

## 开发工作流

1. 明确每个上游通道要发布的 key、类型、缺失语义，并在上游 manifest 的 `outputs[]` 声明。
2. 在 `logic/global_modules/<global_logic_id>/` 创建 `logic.cpp` 和 `logic.json`。
3. 实现 `static void global_xxx(GlobalContext *gctx)`，末尾写
   `REGISTER_GLOBAL_LOGIC(global_xxx);`。
4. 普通业务从 `gctx->inputs()` 读取框架已经过滤的输入，不直接依赖通道私有 state。
5. 跨 tick 状态放 `gctx->state`；周期基于 `timestamp_ms`/`dt_ms`，现实时间基于 `unix_ms`。
6. 参数、事件、上报字段、Action 和模板声明遵循与通道模块相同的 manifest 规则。
7. 给出在 `global.global_logics[]` 创建稳定唯一 `instance_id`、输入通道、兜底周期和视频来源的配置说明；
   `vision develop` 不修改示例或应用配置文件。
8. 校验 catalog、配置和运行日志，再覆盖断流、数据过期、热重载和事件失败测试。

最小骨架：

```cpp
#include <rkvision/global_context.h>

struct TotalState
{
    int64_t last_total = 0;
};

static void global_total(GlobalContext *gctx)
{
    if (!gctx || !gctx->state)
        return;
    if (!*gctx->state)
        *gctx->state = std::make_shared<TotalState>();
    auto &state = *std::static_pointer_cast<TotalState>(*gctx->state);

    int64_t total = 0;
    for (const ChannelInput &input : gctx->inputs())
        total += input.get_int("person_count", 0);
    state.last_total = total;
}

REGISTER_GLOBAL_LOGIC(global_total);
```

`get_int(..., 0)` 把缺失/类型错误按 0 处理；若业务必须区分缺失和合法 0，改用 `read_int()` 并检查
返回值。

## 默认输入规则

`inputs()` 是普通业务的入口：

- `channels` 非空时，只选择这些配置 ID；
- `channels` 为空时，当前调度器遍历应用全部有效通道；
- 未发布、离线或过期的快照会被排除；
- 自动过期阈值是 `max(2000 ms, 3 × 配置的兜底周期)`。

因此，当前运行语义中空 `channels` 不是“永久无输入”。如业务必须明确没有输入，应禁用/移除该
全局实例，而不是依赖空数组。

## 配置和热重载

```json
{
  "global": {
    "global_logics": [
      {
        "instance_id": "aggregate_main",
        "enable": true,
        "logic": "global_person_count_alarm_demo",
        "channels": [0, 1],
        "poll_interval_ms": 100,
        "logic_parameters": {
          "alarm_threshold": 3
        }
      }
    ]
  }
}
```

`instance_id` 必填且在配置内唯一，`poll_interval_ms` 最低为 10 ms。它只控制没有通道更新时的
兜底唤醒；正常数据路径由通道发布立即触发。为防止高频发布持续抢占 CPU，调度器在两次分发之间
保留至少 5 ms。每个启用实例独立运行且同一实例不会并发重入。

回调间隔不是常量。周期业务必须使用 `gctx->timestamp_ms`/`gctx->dt_ms`，不得用 `tick_id`、
调用次数或 `poll_interval_ms` 推算经过时间。调度器只保留每个通道的最新快照；快速连续发布可能
合并到一次回调，通过 `updated_channels`/`missed_revisions` 可以观察版本跨度，但它们不是事件队列。
热重载按稳定 `instance_id` 对比：完全未变化的实例不重启；变化实例停止后重建。仅当 logic 和
channels 均未变化，且参数影响为 `NONE` 或 `PRESERVE_STATE` 时，旧 state 才会带入新实例；
其他变更重置 state。含 `restart_required` 参数的整轮配置更新会在产生模型/流副作用之前拒绝。

## Actions 与事件

全局模块可声明 `actions[]` 并注册
`REGISTER_GLOBAL_LOGIC_ACTION(global_xxx, handler)`。Action 在该实例下一次分发、正常全局函数之前
处理；入队后 logic 改变则丢弃。Web 提交路径是：

```text
POST /api/apps/{app_name}/global-logics/{instance_id}/actions/{action_id}
```

请求体为 `{"payload": {...}}`。HTTP 成功只表示动作已进入当前实例队列，不代表 handler 已执行；
handler 的结果写视觉程序日志。队列当前最多 64 条，溢出会丢弃最旧项。

全局事件使用 `report_event(gctx, request)`。聚合事件通常不设置 `source_channel_id`，只把本次涉及的
通道写入 `evidence_channel_ids`；Web 决定图片使用证据通道、固定通道或所有画布连入通道。
`source_channel_id` 只用于确实由单路通道独立触发的来源元数据，不参与图片或视频选择；事件视频
始终使用明确配置的 `media_source_channel_id`。完整规则见
[事件与上报开发](../rk3588-console-ops/references/event-reporting.md)。

## 按任务加载参考页

- 输入过滤、版本、原始快照、线程和字段：
  [GlobalContext API](references/global-context.md)
- 当前可运行的通道发布 → 全局聚合 → Action → 上报闭环：
  [当前聚合示例](references/aggregate-demo.md)
- 参数与 module manifest：
  [通道模块清单](../rk3588-channel-logic/references/module-manifest.md)

## 验证

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --logic-root ../projects/person_count/logic --check
```

仓库当前没有提交预编译二进制。仅在已经用当前源码得到 `./vision_analysis` 后再运行：

```bash
./vision_analysis --list-global-logics
./vision_analysis --validate-config ../projects/person_count/assets/config_global.json
```

`config_global.json` 是仓库当前的全局逻辑示例；验收具体应用时应换成实际运行配置。运行时还应确认
线程启动日志、有效 `inputs()` 数、断流淘汰、Action 回执、状态在不同实例间隔离，
以及全局回调耗时不会阻塞下一次业务所需调度。应用停机时只能完成静态校验；端到端延迟必须在
目标设备启动实际配置后，用通道发布时间和全局回调时间戳测量，不能只根据配置周期推断。
