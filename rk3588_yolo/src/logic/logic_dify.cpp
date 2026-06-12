/**
 * @file logic_dify.cpp
 * @brief logic_dify —— 检测到目标即按间隔把抓拍 + 提示词上报 Dify。
 */
#include "logic_common.h"

struct DifyState
{
    uint64_t last_upload_ms = 0;
    int first = 1;
};

static void logic_dify(ChannelContext *ctx)
{
    if (!ctx || !ctx->results || ctx->results->empty())
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<DifyState>();
    auto &s = *std::static_pointer_cast<DifyState>(*ctx->state);

    /* 上报间隔(秒)→ms：每帧从 ctx->config 现读以支持热重载 */
    const uint64_t interval_ms =
        (uint64_t)std::max(1, ctx->config ? ctx->config->report_interval_sec : 5) * 1000ULL;

    if (s.first)
    {
        s.last_upload_ms = ctx->timestamp_ms;
        s.first = 0;
        return;
    }

    if (ctx->timestamp_ms - s.last_upload_ms < interval_ms)
        return;
    if (!ctx->frame || ctx->frame->empty())
        return;

    const char *prompt = (ctx->config && !ctx->config->dify_prompt.empty())
                             ? ctx->config->dify_prompt.c_str()
                             : "无提示词";

    char event_id[64];
    snprintf(event_id, sizeof(event_id), "ch%d_f%lld_t%llu",
             ctx->chnId, (long long)ctx->frame_id, (unsigned long long)ctx->timestamp_ms);

    const char *dify_url = (ctx->config && !ctx->config->dify_api_url.empty()) ? ctx->config->dify_api_url.c_str() : nullptr;
    const char *dify_key = (ctx->config && !ctx->config->dify_api_key.empty()) ? ctx->config->dify_api_key.c_str() : nullptr;
    dify_uploader_enqueue(*ctx->frame, prompt, event_id, dify_url, dify_key);

    s.last_upload_ms = ctx->timestamp_ms;
}

REGISTER_LOGIC("logic_dify", logic_dify);
