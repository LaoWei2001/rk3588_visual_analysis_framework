# logic_dify — Dify 二次核验（周期上报）

- **上报**：dify（`dify_uploader_enqueue`）
- **可调参数**：无（间隔在代码里写死 10s）
- **用到的能力**：跨帧状态计时、Dify 上报、`dify_prompt`

## 做什么
每隔固定间隔（10 秒）把当前帧发给 Dify 工作流做 AI 二次核验。用 `ctx->state` 记上次上报时间做限频；首帧只记时不发。提示词取本通道 `config.dify_prompt`，地址/密钥取 `config.dify_api_url/key`。

## 完整实现
```cpp
struct DifyState {
    uint64_t last_upload_ms = 0;
    int first = 1;
};

static void logic_dify(ChannelContext *ctx)
{
    if (!ctx || !ctx->results || ctx->results->empty())
        return;

    if (!*ctx->state) *ctx->state = std::make_shared<DifyState>();
    auto &s = *std::static_pointer_cast<DifyState>(*ctx->state);

    static constexpr uint64_t INTERVAL_MS = 10000;

    if (s.first) {                                   // 首帧只记基准时间
        s.last_upload_ms = ctx->timestamp_ms;
        s.first = 0;
        return;
    }
    if (ctx->timestamp_ms - s.last_upload_ms < INTERVAL_MS) return;   // 限频
    if (!ctx->frame || ctx->frame->empty()) return;

    const char *prompt = (ctx->config && !ctx->config->dify_prompt.empty())
                             ? ctx->config->dify_prompt.c_str() : "无提示词";

    char event_id[64];
    snprintf(event_id, sizeof(event_id), "ch%d_f%lld_t%llu",
             ctx->chnId, (long long)ctx->frame_id, (unsigned long long)ctx->timestamp_ms);

    const char *dify_url = (ctx->config && !ctx->config->dify_api_url.empty()) ? ctx->config->dify_api_url.c_str() : nullptr;
    const char *dify_key = (ctx->config && !ctx->config->dify_api_key.empty()) ? ctx->config->dify_api_key.c_str() : nullptr;
    dify_uploader_enqueue(*ctx->frame, prompt, event_id, dify_url, dify_key);

    s.last_upload_ms = ctx->timestamp_ms;
}
```

## 接线
- 注册：`register_logic("logic_dify", logic_dify);`
- logics.json：`{ "name": "logic_dify", "label": "Dify 二次核验", "report": "dify", "params": [] }`
- 网页给该通道连「上报配置」节点选 Dify、填地址/密钥/提示词。

## 复用提示
"每隔 N 秒发一次 AI 核验"用这个。想把间隔做成可调参数，把 `INTERVAL_MS` 换成 `ctx->config->dify_interval_ms`，并走"可调参数三处对齐"（见 `upload-and-wiring.md`）。
