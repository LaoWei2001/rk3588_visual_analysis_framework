#include "logic/core/logic_common.h"

static void logic_course_02(ChannelContext *ctx)
{
    // 空指针验证
    if (!ctx || !ctx->frame || ctx->frame->empty())
    {
        return;
    }
    char str1[128];
    char str2[128];
    sprintf(str1, "当前通道号为:%d,时间为%s", ctx->chnId, ctx->time_str().c_str());
    draw_text(ctx, str1, cv::Point(20, 100), cv::Scalar(255, 255, 255), 0.8, 1, DrawCommand::ALL);
    sprintf(str2, "测试文本为:%s", ctx->param_string("test_string").c_str());
    draw_text(ctx, str2, cv::Point(20, 120), cv::Scalar(255, 255, 255), 0.8, 1, DrawCommand::ALL);
}

REGISTER_LOGIC(logic_course_02);
