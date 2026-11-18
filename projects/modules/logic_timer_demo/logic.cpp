#include <channel.h>
#include <cstdio>

struct StartTimes
{
    uint64_t analysis;
    uint64_t real;
};

static void logic_timer_demo(ChannelContext *ctx)
{
    // 每路只记录一次起点；两种时间都由框架提供。
    const auto *start = ctx->get_state<StartTimes>(StartTimes{ctx->timestamp_ms, ctx->real_time_ms});
    char text[64];

    std::snprintf(text, sizeof(text), "分析 %.1f 秒", (ctx->timestamp_ms - start->analysis) / 1000.0);
    draw_text(ctx, text, {470, 36}, {255, 255, 255}, 0.7, 1, DrawCommand::DISPLAY, true);

    std::snprintf(text, sizeof(text), "现实 %.1f 秒", (ctx->real_time_ms - start->real) / 1000.0);
    draw_text(ctx, text, {470, 70}, {255, 255, 255}, 0.7, 1, DrawCommand::DISPLAY, true);
}

REGISTER_LOGIC(logic_timer_demo);
