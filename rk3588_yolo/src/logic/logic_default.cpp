/**
 * @file logic_default.cpp
 * @brief logic_default —— 空逻辑，对检测结果不做任何处理
 */
#include "logic_common.h"

static void logic_default(ChannelContext *ctx)
{
    (void)ctx;
}

REGISTER_LOGIC("logic_default", logic_default);
