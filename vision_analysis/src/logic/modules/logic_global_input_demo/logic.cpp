#include "logic/core/logic_common.h"

#include <algorithm>
#include <string>

/* 同一个模块可放到任意多个视频通道，每个实例使用自己的参数。 */
static void logic_global_input_demo(ChannelContext *ctx)
{
    if (!ctx || !ctx->results)
        return;

    const std::string target_label = ctx->param_string("target_label");
    const int64_t local_threshold = std::max<int64_t>(1, ctx->param_int("local_count_threshold"));
    const int64_t target_count = ctx->target_count(target_label.c_str());

    /* 公开给下游全局逻辑的同帧业务变量。 */
    ctx->publish_int("target_count", target_count);
    ctx->publish_bool("local_alarm", target_count >= local_threshold);
    ctx->publish_number("risk_ratio", static_cast<double>(target_count) / local_threshold);
}

REGISTER_LOGIC(logic_global_input_demo);
