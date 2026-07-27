// 课程1：自定义文字和图形叠加
// 实现效果:在屏幕上显示出文字和图形
// 难度:★☆☆☆☆
#include "logic/core/logic_common.h"

static void logic_course_01(ChannelContext *ctx)
{
    // 空指针验证
    if (!ctx || !ctx->frame || ctx->frame->empty())
        return;

    // 叠加自定义文字, 可使用中文
    draw_text(ctx, "course1 logic running, 正在运行", cv::Point(30, 50), cv::Scalar(255, 0, 0), 1, 1,
              DrawCommand::ALL);
    // 画一个圆
    draw_circle(ctx, cv::Point(320, 320), 100, cv::Scalar(255, 0, 0));
}

REGISTER_LOGIC(logic_course_01);
