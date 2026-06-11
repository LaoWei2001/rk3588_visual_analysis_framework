# logic_server — 服务器告警上报

- **上报**：server（`alarm_uploader_enqueue`）
- **可调参数**：无
- **用到的能力**：方案2 每通道地址、最简上报

## 做什么
最简单的"有检测结果就上报到 HTTP 服务器"。一旦本帧有任何检测结果，就把当前帧打包、带上本通道的 `server_url` 推到 `server_queue`，告警类型 `"intrusion"`。

> ⚠ 注意：它**没有限频**——只要有结果就每帧上报。真实场景几乎一定要加限频（用 `ctx->timestamp_ms` 卡间隔），见 `logic_hook` / `examples` 里的限频写法。这个 logic 更多是"最小上报骨架"的演示。

## 完整实现
```cpp
static void logic_server(ChannelContext *ctx)
{
    if (!ctx || !ctx->results || ctx->results->empty())
        return;
    if (!ctx->frame)
        return;
    alarm_uploader_enqueue(*ctx->frame, *ctx->frame, ctx->chnId, "intrusion",
                           ctx->config ? ctx->config->server_url.c_str() : nullptr);
}
```

## 接线
- 注册：`register_logic("logic_server", logic_server);`
- logics.json：`{ "name": "logic_server", "label": "服务器告警上报", "report": "server", "params": [] }`
- 用户需在网页给该通道连一个「上报配置」节点并填 HTTP 地址（写进该通道 `config.json` 的 `server_url`）。

## 复用提示
要做"检测到 X 就上报服务器"，从这里起步，加上：① 类别过滤 `r.label != "person"` ② ROI 过滤 ③ 限频。
