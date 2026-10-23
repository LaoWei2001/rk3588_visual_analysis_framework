#include <channel.h>

/* Default channel logic: intentionally does nothing. */
static void logic_default(ChannelContext *ctx)
{
    printf("1234545\n");
}

REGISTER_LOGIC(logic_default);
