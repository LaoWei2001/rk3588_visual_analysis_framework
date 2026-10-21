#include <channel.h>

/* Default channel logic: intentionally does nothing. */
static void logic_default1019(ChannelContext *ctx)
{
    printf("每一帧的编号:%ld\n",ctx->frame_id);
}

REGISTER_LOGIC(logic_default1019);
