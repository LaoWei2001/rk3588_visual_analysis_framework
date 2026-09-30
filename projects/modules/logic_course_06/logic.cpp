// 课程6:如何保存跨帧状态变量(为什么要引入跨帧的变量? 跟普通的变量有什么区别? 要解决什么问题?)
// 实现效果:视频画面上显示当前通道逻辑的总调用次数
// 难度:★★★☆☆

#include "logic/core/logic_common.h"

// 为什么要把变量保存在这个结构体中而不是保存在logic_course_06这个函数中?
// 如果我把call_count这个变量直接定义在logic_course_06中进行运算会发生什么?（提示：变量生命周期）
// 如果我不使用*ctx->state = std::make_shared<DemoState>();会发生什么（提示：如果多通道共用一个逻辑）
struct DemoState
{
    // 当前通道逻辑的累计调用次数
    uint64_t call_count = 0;
};

static void logic_course_06(ChannelContext *ctx)
{
    // 空指针验证
    if (!ctx || !(ctx->state))
    {
        return;
    }
    if (!*ctx->state)
    {
        // 程序启动时初始化状态
        *ctx->state = std::make_shared<DemoState>();
    }
    // 把无类型状态恢复为 DemoState
    std::shared_ptr<DemoState> state = std::static_pointer_cast<DemoState>(*ctx->state);
    (state->call_count)++;

    char num_display[128];
    sprintf(num_display, "当前通道逻辑的累计调用次数:%ld", state->call_count);
    draw_text(ctx, num_display, cv::Point(20, 135), cv::Scalar(255, 255, 255), 0.8, 1, DrawCommand::ALL);
}

REGISTER_LOGIC(logic_course_06);
