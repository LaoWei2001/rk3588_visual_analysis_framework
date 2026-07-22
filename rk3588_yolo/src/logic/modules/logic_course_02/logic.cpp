#include "logic/core/logic_common.h"

static void logic_course_02(ChannelContext *ctx)
{
    // 空指针验证
    if (!ctx || !ctx->frame || ctx->frame->empty())
    {
        return;
    }
    char str[128];
    sprintf(str, "当前通道号为:%d,时间为%s", ctx->chnId, ctx->time_str().c_str());
    draw_text(ctx, str, cv::Point(20, 100), cv::Scalar(255, 255, 255), 0.8, 1,
              DrawCommand::ALL);

}

REGISTER_LOGIC(logic_course_02);
