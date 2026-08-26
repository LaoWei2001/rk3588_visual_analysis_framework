---
name: rk3588-channel-logic
description: >-
  Develop, review, or explain a per-channel C++ logic module in this RK3588
  visual-analysis framework. Use for ChannelContext, detections, ROI, drawing,
  per-channel state, typed outputs, logic parameters, Web actions, or channel
  event reporting. Read the current module manifest together with its C++ and
  verify the generated catalog; do not use historical module names or a removed
  direct frame field.
---

# RK3588 通道逻辑开发

本 Skill 是单通道业务逻辑的工作入口，同时服务大模型编程和二次开发者。它以当前源码为准：

- 公共接口：`vision_analysis/src/logic/core/channel_logic.h`、`logic_action.h`、`logic_outputs.h`；
- 模块目录：`vision_analysis/src/logic/modules/<logic_id>/`；
- 注册与清单校验：`vision_analysis/scripts/generate_logics_catalog.py`；
- 事件接口：`vision_analysis/src/event/event_report.h`。

不要从旧文档推断接口。当前 `ChannelContext` 没有 `frame` 成员；需要像素时调用
`model_frame()` 或 `source_frame()`。

## 何时使用

使用本 Skill 处理逐业务帧运行、只属于一个视频通道的规则，包括检测结果筛选、ROI、跟踪结果、
跨帧状态、绘制、向全局逻辑发布变量、Action 和事件创建。

以下任务转到其他 Skill：

- 跨多个通道组合判断：[`rk3588-global-logic`](../rk3588-global-logic/SKILL.md)；
- 上传协议、Web 接线与发件箱：[`rk3588-console-ops`](../rk3588-console-ops/SKILL.md)；
- 引擎线程、配置热重载或公共 API：[`rk3588-src-modules`](../rk3588-src-modules/SKILL.md)。

## Logic-only 写入规则

通过 `develop_feature` 运行时，只能写入 `vision_analysis/src/logic/modules/**` 和
`vision_analysis/src/logic/global_modules/**`。本 Skill 的通道产物必须全部放在所属
`modules/<logic_id>/` 内；上报模板也放在该目录的 `report_templates/`。公共 `logic/core`、配置、测试、
Web、服务、文档、脚本和生成物全部只读。无法使用现有公共接口和 manifest 完成时立即停止，不新增公共
API，也不请求扩大权限。机械执行规则见
[`Logic-only 写入边界`](../rk3588-feature-wizard/references/write-boundary.md)。

## 开始前的取证顺序

1. 打开目标模块的 `logic.cpp` 和 `logic.json`，两者必须一起检查。
2. 打开上述公共头文件确认当前签名，不凭记忆补字段。
3. 检查运行配置中该通道的模型 ID、标签、ROI 名称、logic 参数和上报策略。
4. 运行清单校验，确认当前二进制应注册的 logic ID。
5. 若需求含远端上报，再读取[事件与上报开发](../rk3588-console-ops/references/event-reporting.md)。

## 新模块工作流

1. 在 `vision_analysis/src/logic/modules/logic_xxx/` 新建 `logic.cpp` 和 `logic.json`。
2. 实现 `static void logic_xxx(ChannelContext *ctx)`，包含 `logic/core/logic_common.h`。
3. 文件末尾写 `REGISTER_LOGIC(logic_xxx);`。函数标识符就是配置、Web 和 API 使用的唯一 ID。
4. 在 `logic.json` 声明参数、事件、上报字段、输出和 Action；不要手写 `name`。
5. 只用 `ctx->param_*()` 读取模块参数，不为普通业务参数扩展中央 `ChannelConfig`。
6. 用 `ctx->state` 保存每通道跨帧状态；不能用可变函数级 `static` 共享业务状态。
7. 若发布变量，C++ 的 `publish_*()` key/type 必须与 `logic.json.outputs[]` 一致。
8. 若创建事件，事件 ID 和字段必须与 `event_types[]`、`report_fields[]` 一致。
9. 运行只读生成器校验；在隔离环境可执行的构建和测试完成后，再给出部署与真实输入验收步骤。

最小骨架：

```cpp
#include "logic/core/logic_common.h"

struct XxxState
{
    uint64_t seen_frames = 0;
};

static void logic_xxx(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results)
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<XxxState>();
    auto &state = *std::static_pointer_cast<XxxState>(*ctx->state);
    ++state.seen_frames;

    const int count = ctx->target_count("person");
    ctx->publish_int("person_count", count);
    const std::string status = "count=" + std::to_string(count);
    draw_text(ctx, status.c_str(), {20, 30});
}

REGISTER_LOGIC(logic_xxx);
```

这里的 `person` 只是示例值，实际值必须与目标模型的标签文件完全一致；相应 manifest 还必须声明
`person_count` 为 `integer` 输出。

## 运行语义

- 同一通道的业务回调由框架串行化；跟踪器先于 logic 执行。
- 推理通道收到当帧推理/跟踪结果；未开启推理的通道仍按业务节拍调用 logic，但 `results` 为空。
- 未配置 logic 时，框架仍发布帧和结果，只是不执行业务模块。
- 每次回调重新创建 outputs 和绘制指令，logic 返回后与帧、结果一起提交。
- Action 在目标通道下一次业务帧、logic 回调之前执行；若排队后切换了 logic，旧 Action 会被丢弃。
- `timestamp_ms` 是单调毫秒，用于间隔；`unix_ms` 是 epoch 毫秒，用于现实时间。
- logic 位于实时路径，不执行 HTTP、长时间磁盘 I/O、阻塞等待或视频编码。

## 按任务加载参考页

- 上下文、坐标、结果、ROI、绘制、状态和输出：
  [ChannelContext API](references/channel-context.md)
- `logic.json` Schema、热重载元数据和生成器约束：
  [模块清单](references/module-manifest.md)
- 自定义按钮、handler 和 HTTP API：
  [通道 Action](references/actions.md)
- 当前确实存在的课程与业务示例：
  [现有模块索引](references/current-examples.md)
- 事件、媒体、合并、投递契约：
  [事件与上报开发](../rk3588-console-ops/references/event-reporting.md)

## 必做验证

```bash
cd vision_analysis
python3 scripts/generate_logics_catalog.py --check
```

有可运行二进制时再核对：

```bash
./vision_analysis --list-logics
./vision_analysis --validate-config ./assets/config_6.json
```

`config_6.json` 是仓库内现存的可复制示例；验收具体应用时，`--validate-config` 后应改为实际准备运行的配置。编译和打包
流程见 [`build-rk3588-vision-app`](../build-rk3588-vision-app/SKILL.md)。

## 完成标准

- 注册 ID、目录、配置中的 `channels[].logic` 和 Web 清单一致；
- 参数/outputs/事件/字段/Action 均在同模块 `logic.json` 声明并通过生成器静态检查；
- 像素指针生命周期、模型/源坐标、状态归属和 Action 时序处理正确；
- 每帧行为有限时，告警有闩锁、冷却或业务去重；
- `report_event().accepted()` 只被解释为本地事件请求已受理，不被写成远端成功；
- 验收覆盖无检测、无 ROI、断流、热重载、Action 排队和上报失败等实际边界。
