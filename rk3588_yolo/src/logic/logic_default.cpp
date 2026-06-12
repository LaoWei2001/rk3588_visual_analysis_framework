/**
 * @file logic_default.cpp
 * @brief logic_default —— 空逻辑(不画不报, 透传)。
 */
#include "logic_common.h"

static void logic_default(ChannelContext *ctx)
{
    (void)ctx;
}

REGISTER_LOGIC("logic_default", logic_default);
