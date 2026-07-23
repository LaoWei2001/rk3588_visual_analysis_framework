# 通道动作控制

这套机制用于让 Web 端 / 外部设备向 C++ 主程序按通道投递控制动作。

系统只有一套通道身份：`channel_id = config.channels[].id`。该 ID 同时用于
HTTP/Socket、logic、告警、录像和 C++ 固定数组索引。配置加载器保证它唯一且在
`[0, MAX_CHANNEL_NUM)` 内。显示宫格只根据配置顺序计算位置，不会产生第二套 ID。

## 执行链路

`Web 按钮 / 外部HTTP → FastAPI(:8080) → Unix Socket(run.control.sock) → handle_client() → [系统级动作直接处理 | 业务级动作入队 → 下一帧消费]`

## 两种动作类型

### 业务级动作（Logic-level）

归属某个特定 logic 模块，在 `invoke_channel_logic()` 中执行，可安全访问 `ctx->state / frame / results`。

- 通道必须有对应 logic 才能处理
- 通道热切换 logic 后，旧 logic 排队的动作被丢弃

### 系统级动作（System-level）

不归属任何 logic，在 `handle_client()` 中直接处理，修改 `ChannelState` 后立即响应。

- 不需要通道有 logic 模块
- 对所有 logic 类型通用
- 不经过动作队列，无帧延迟
- 新增只需在 `handle_client()` 加一个 `strcmp` 分支（**必须放在 logic 检查之前**）

当前系统级动作：
- `infer_toggle` — 翻转本通道 NPU 推理启停（修改 `ChannelState::infer_runtime_enable`）

## 新增一个业务级动作（带按钮）

1. 在 `src/logic/modules/<module_dir>/logic.json` 中新增 `actions`
2. 在对应 `logic_xxx.cpp` 里实现一个 `ChannelActionFunc`
3. 用 `REGISTER_LOGIC_ACTION(logic_xxx, your_action_func);` 注册，第一参数是已传给 `REGISTER_LOGIC(logic_xxx)` 的入口函数

`actions` 示例：

```json
{
  "id": "start_new_run",
  "label": "开始新一轮",
  "style": "primary",
  "confirm": "确认清空当前 SOP 状态并重新开始吗？",
  "help": "人工手动触发新工序"
}
```

`logic` 侧示例：

```cpp
static ChannelActionResult logic_demo_action(ChannelContext *ctx, const ChannelAction &action)
{
    ChannelActionResult result;
    if (action.name == "reset")
    {
        *ctx->state = std::make_shared<DemoState>();
        result.handled = true;
        result.message = "reset done";
        return result;
    }
    result.message = "unsupported action";
    return result;
}

REGISTER_LOGIC_ACTION(logic_demo, logic_demo_action);
```

## 新增一个系统级动作

在 `channel_control.cpp` 的 `handle_client()` 中，**`current_logic_name()` 检查之前**添加拦截：

```cpp
if (strcmp(action_name, "my_system_action") == 0)
{
    pthread_mutex_lock(&g_pCtrl->chn_mtx[channel_id]);
    // 直接修改 ChannelState
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[channel_id]);

    send_all(client_fd, make_response(true, req_id, channel_id,
                                      action_name, "", "ok"));
    cJSON_Delete(root);
    return;   // 直接返回，不经过 logic 分发
}
```

如果需要在 Web 界面显示按钮，还需在 `logics.json` 的任一 logic 的 `actions` 中声明（或加入全局 system_actions 列表）。

## 外部 API 调用

任何能发 HTTP POST 的设备都可以触发动作：

```bash
# 先获取 token
curl -X POST http://<IP>:8080/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"root","password":"<pwd>"}'

# 触发动作
curl -X POST "http://<IP>:8080/api/apps/<app>/channels/<ch>/actions/<action>" \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"payload":{}}'
```

其中 `<ch>` 是配置中的稳定通道 ID，不是 `channels[]` 数组下标。

## 默认行为

如果某个 logic 没有在 `logics.json` 里声明动作，Web 端不会显示按钮。系统级动作仍可通过 API 调用。

- 当前 logic 实现了动作处理：可按你的自定义语义消费
- 当前 logic 未实现动作处理：Web 会收到"current logic has no action handler"
- 系统级动作：不需要 logic，始终可用
