# logic_dify — Dify 二次核验（周期上报）

- **上报**：dify（`dify_uploader_enqueue`）
- **可调参数**：`report_interval_sec`（int，上报间隔秒，默认 5；与 logic_server 共用）
- **用到的能力**：跨帧状态计时、Dify 上报、`dify_prompt`

## 做什么
每隔 `report_interval_sec` 秒（默认 5s，可热重载）把当前帧发给 Dify 工作流做 AI 二次核验。用 `ctx->state` 记上次上报时间做限频；首帧只记时不发。间隔每帧从 `ctx->config->report_interval_sec` 现读。提示词取本通道 `config.dify_prompt`，地址/密钥取 `config.dify_api_url/key`。

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

    // 上报间隔(秒)→ms：每帧现读以支持热重载
    const uint64_t interval_ms =
        (uint64_t)std::max(1, ctx->config ? ctx->config->report_interval_sec : 5) * 1000ULL;

    if (s.first) {                                   // 首帧只记基准时间
        s.last_upload_ms = ctx->timestamp_ms;
        s.first = 0;
        return;
    }
    if (ctx->timestamp_ms - s.last_upload_ms < interval_ms) return;   // 限频
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
- 文件 / 注册：独立文件 `src/logic/logic_dify.cpp`（顶部 `#include "logic_common.h"`），文件末尾自注册 `REGISTER_LOGIC("logic_dify", logic_dify);`。
- logics.json：`{ "name": "logic_dify", "label": "Dify 二次核验", "report": "dify", "params": [ { "key": "report_interval_sec", "type": "int", "label": "上报间隔(秒)", "default": 5, "min": 1, "max": 3600 } ] }`
- 网页给该通道连「上报配置」节点选 Dify、填地址/密钥/提示词。

## 复用提示
"每隔 N 秒发一次 AI 核验"用这个。间隔已是可调参数 `report_interval_sec`（每帧从 `ctx->config` 现读、自动热重载）；若想用独立于 server 的间隔，照"可调参数三处对齐"另加一个 `dify_interval_sec` 字段（见 `adding-config-parameter.md`）。
