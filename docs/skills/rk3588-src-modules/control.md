# `src/control`：通道动作控制

该模块把 Web 或外部设备发来的通用动作转交给 C++。协议链路为：

```text
Web/API -> Unix Socket -> channel_control -> 系统动作直接执行
                                      \-> 业务动作按通道入队 -> 下一次 logic 调用前执行
```

## Socket 协议

默认 socket 是 `/tmp/rk3588_channel_control_<pid>.sock`，可由 `RK_CHANNEL_CONTROL_SOCKET` 覆盖，权限为 `0660`。一次连接接收一个最大 64 KiB 的 JSON 请求：

```json
{"request_id":"r1","channel_id":0,"action":"clear_state","payload":{}}
```

`channel_id` 是系统唯一的通道身份，始终等于 `config.channels[].id`。响应为单行
JSON，不再暴露内部槽位或别名；每通道队列最多 64 条，满时丢弃最旧动作。

## 两类动作

系统级动作在 socket 线程直接处理，不依赖 logic。当前只有 `infer_toggle`，它翻转 `ChannelState::infer_runtime_enable`，仅停止/恢复 NPU 推理，画面与非推理 logic 仍运行。

业务级动作入队时记录当前 `logic_name`。`channel_pipeline` 在下一帧调用 logic 前取走动作；若期间切换了 logic，旧 logic 的动作不会误交给新 logic。动作处理函数签名为 `ChannelActionResult(ChannelContext *, const ChannelAction *)`。

## 新增 Web 业务按钮

1. 在 `src/logic/modules/logic_xxx/logic.json` 的 `actions` 中声明 `id`、`label`，可选 `style`、`confirm`、`help`。
2. 在同模块 C++ 中实现动作函数，先检查 `action` 指针，再处理 `action->name`/`action->payload_json`。
3. 使用 `REGISTER_LOGIC_ACTION(logic_xxx, handler)` 注册；第一参数必须是已传给 `REGISTER_LOGIC(logic_xxx)` 的入口函数。

动作函数与逐帧 logic 在同一通道处理路径执行，可安全修改 `*ctx->state`，但不得阻塞做长耗时网络或磁盘操作。完整 Web/API 侧约定见 `../rk3588-channel-logic/`。

新增系统级动作时应在 `handle_client()` 的 logic 检查之前处理并立即响应；若还要显示 Web 按钮，仍需在相应 logic 元数据中声明。

## 生命周期

`channel_control_init()` 建 socket 并启动 server thread；失败只会禁用按钮链路，主程序可继续。`channel_control_deinit()` 停线程、关闭并删除 socket、清空所有队列。
