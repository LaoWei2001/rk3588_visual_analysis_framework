// 课程2：ctx变量调用
// 实现效果:在画面上显示自定义的文字(注意与课程1的区别),在控制台显示自定义的数字
// 难度:★★☆☆☆
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
    float test;
    // ChannelContext结构体中信息的调用
    sprintf(str1, "当前通道号为:%d,时间为%s", ctx->chnId, ctx->time_str().c_str());
    draw_text(ctx, str1, cv::Point(20, 100), cv::Scalar(255, 255, 255), 0.8, 1, DrawCommand::ALL);

    // 开发者添加的支持web界面修改的变量的调用(字符串)
    sprintf(str2, "测试文本为:%s", ctx->param_string("test_string").c_str());
    draw_text(ctx, str2, cv::Point(20, 135), cv::Scalar(255, 255, 255), 0.8, 1, DrawCommand::ALL);

    // 开发者添加的支持web界面修改的变量的调用(小数)
    test = ctx->param_float("test_num");
    printf("自定义的变量值为：%f\n", test);
}

REGISTER_LOGIC(logic_course_02);
