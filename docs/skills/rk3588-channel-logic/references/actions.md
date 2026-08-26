# 通道 Action

Action 用于 Web 或外部 API 向当前通道 logic 投递“动作名 + JSON 参数”，适合复位、切换模式、
改变计数或设置一次性请求。它不是在 HTTP 请求线程里直接调用业务代码。

## 数据和注册

`LogicAction` 当前包含：

- `request_id`：请求标识；
- `name`：必须匹配 manifest 的 `actions[].id`；
- `payload_json`：原始 JSON 字符串，默认 `{}`；
- `logic_name`：入队时的 logic ID，用于防止热切换后误投；
- `received_unix_ms`：接收时 epoch 毫秒。

handler 返回 `LogicActionResult { handled, message }`。注册方式：

```cpp
static LogicActionResult handle_action(ChannelContext *ctx,
                                       const LogicAction *action)
{
    if (!ctx || !ctx->state || !action)
        return {false, "invalid context"};
    if (action->name != "reset")
        return {false, "unknown action"};

    if (*ctx->state)
        std::static_pointer_cast<MyState>(*ctx->state)->latched = false;
    return {true, "reset"};
}

REGISTER_LOGIC(logic_xxx);
REGISTER_LOGIC_ACTION(logic_xxx, handle_action);
```

## 时序和并发

Action 入队后，在该通道下一次业务帧中、正常 logic 回调之前执行。因此 handler 与该通道的
logic 状态访问处于同一串行执行路径。排队后若通道切换为别的 logic，框架会丢弃旧请求，避免
发给新模块。

handler 应快速完成。需要当前帧才能创建事件时，handler 只在 state 中设置 pending 标志，随后
由同一业务帧的 logic 构造 `EventRequest`。不要在 handler 中联网、等待或做长耗时工作。

## Web/API 契约

动作列表来自当前 App 的生成清单：

```text
GET /api/apps/{app_name}/logic-actions
```

通道动作提交路径：

```text
POST /api/apps/{app_name}/channels/{channel_id}/actions/{action_id}
```

当前控制台特意将通道/全局 Action 的 POST 路径设为免登录入口，便于外部控制器调用；这意味着
部署者必须通过可信网络、反向代理或外围访问控制保护它，不能把“免登录”误写成“无需安全边界”。
动作列表 GET 以及其他普通 API 仍受控制台鉴权约束。

当前 HTTP 请求体必须包一层 `payload`：

```json
{"payload": {"value": 1}}
```

其中内层对象会序列化到 `LogicAction.payload_json`。POST 返回的 `ok: true` / `message: "accepted"`
只表示 control socket 已把动作放入队列，不是 handler 的 `LogicActionResult`；handler 结果只写视觉
程序日志。每通道队列和每全局实例队列当前最多 64 条，溢出时丢弃最旧项。入口只确认当前 logic
注册了 handler，不会按 manifest 预先拒绝未知 action ID，因此 handler 仍必须自行校验 `name`。

`infer_toggle` 是 control 层保留的系统级通道动作：它不经过模块 handler，也不在模块 `actions[]`
声明，收到后立即切换该通道运行时推理开关。业务模块不要复用这个 ID。

## 当前可核对示例

- `logic_course_07`：`increment`、`decrement`、`change`，演示状态修改；
- `logic_relay`：`toggle`，演示按钮控制 GPIO。

这两个示例都必须连同各自 `logic.json` 阅读，不能只复制 handler。`logic_course_09` 当前是空
骨架，虽有“按钮上报图片”的标签，但并未声明 Action 或实现上报，不能作为可运行范例。
