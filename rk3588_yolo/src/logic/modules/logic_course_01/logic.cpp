#include "logic/core/logic_common.h"

static void logic_course_01(ChannelContext *ctx)
{
    // 空指针验证
    if (!ctx || !ctx->frame || ctx->frame->empty())
        return;

    // 叠加自定义文字, 可使用中文
    draw_text(ctx, "course logic running, 正在运行", cv::Point(20, 30), cv::Scalar(255, 255, 255), 1, 1,
              DrawCommand::ALL);
    draw_circle(ctx, cv::Point(20, 30), 1, cv::Scalar(255, 255, 255));
}

REGISTER_LOGIC(logic_course_01);
