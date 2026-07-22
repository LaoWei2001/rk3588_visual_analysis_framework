#include "logic/core/logic_common.h"

/* Default channel logic: intentionally does nothing. */
static void logic_default(ChannelContext *)
{
}

REGISTER_LOGIC(logic_default);
