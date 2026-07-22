# logic_button_demo — Web 动作与手动告警演示

- 源码：`rk3588_yolo/src/logic/modules/logic_button_demo/logic.cpp`
- 告警类型：`button_demo_manual`
- 注册：`REGISTER_LOGIC` + `REGISTER_LOGIC_ACTION`

该示例展示 Web 按钮如何修改 `ctx->state`，以及为什么依赖当前帧的告警要采用 pending 模式。

## 当前动作

动作和参数列表以同模块 `logic.json` 为准，打包时自动聚合给 Web。它包括计数、短脉冲、保持叠加、复位和手动告警等演示动作。handler 用 `button_demo_state(ctx)` 获取本通道状态，只执行快速字段更新。

`trigger_alarm` 不在 handler 中直接创建媒体，而是设置：

```cpp
state.pending_alarm = true;
```

下一次 `logic_button_demo(ctx)` 拿到当前帧后调用：

```cpp
const std::string event_id = report_alarm(
    ctx,
    "button_demo_manual",
    "Web button triggered demo alarm",
    {
        alarm_field("last_action", state.last_action),
        alarm_field("press_count", state.press_count),
        alarm_field("alarm_count", state.alarm_count + 1),
        alarm_field("hold_enabled", state.hold_enabled),
    });
```

只有返回非空事件 ID 时才增加 `alarm_count`。返回空表示未配置 delivery 或事件创建失败，并会显示对应消息。

四个字段已经在模块 `logic.json.report_fields` 声明，并会聚合到 Web 清单。按钮声明、Socket/FIFO 链路、payload 校验和 handler 约束详见 `../custom-button-actions.md`。
