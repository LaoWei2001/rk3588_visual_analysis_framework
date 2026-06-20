/**
 * @file logic_server.cpp
 * @brief logic_server —— 检测到目标即按间隔上报抓拍到服务器(intrusion)。
 */
#include "logic_common.h"

struct ServerState
{
    uint64_t last_upload_ms = 0;
};

static void logic_server(ChannelContext *ctx)
{
    if (!ctx || !ctx->results || ctx->results->empty())
        return;
    if (!ctx->frame)
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<ServerState>();
    auto &s = *std::static_pointer_cast<ServerState>(*ctx->state);

    /* 上报间隔(秒)→ms：每帧从 ctx->config 现读以支持热重载；首次(last=0)立即上报，之后按间隔冷却 */
    const uint64_t interval_ms =
        (uint64_t)std::max(1, ctx->config ? ctx->config->report_interval_sec : 5) * 1000ULL;
    if (ctx->timestamp_ms - s.last_upload_ms < interval_ms)
        return;

    alarm_uploader_enqueue(*ctx->frame, *ctx->frame, ctx->chnId, "intrusion",
                           ctx->config ? ctx->config->server_url.c_str() : nullptr);
    s.last_upload_ms = ctx->timestamp_ms;
}

REGISTER_LOGIC("logic_server", logic_server);
