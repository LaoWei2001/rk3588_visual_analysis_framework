#include "logic/core/logic_common.h"

/* Default channel logic: intentionally does nothing. */
static void logic_default(ChannelContext *ctx)
{
}

REGISTER_LOGIC(logic_default);
