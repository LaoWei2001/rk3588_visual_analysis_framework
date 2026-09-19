#include <rkvision/logic.h>

static void logic_example(ChannelContext *ctx)
{
    if (!ctx) return;
    draw_text(ctx, "RK Vision", {20, 30});
}

REGISTER_LOGIC(logic_example);
