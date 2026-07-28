// 课程7:如何添加一个带自定义功能的按钮
// 按钮控制算法逻辑的本质是什么？按下按钮后，算法逻辑是怎么接收到信号的
// 难度:★★★☆☆

#include "logic/core/logic_common.h"

// 跨帧状态
struct ButtonDemoState
{
    int number = 0;
    int is_first = 0;
};

// 这个函数一定要有返回值，返回为空也行，不然程序会卡死
static ChannelActionResult logic_course_07_action(ChannelContext *ctx, const ChannelAction *action)
{
    if (!ctx || !ctx->state || !action)
    {
        return {};
    }
    if (!*(ctx->state))
    {
        // 第一次按按钮的时候,初始化状态
        *(ctx->state) = std::make_shared<ButtonDemoState>();
    }
    std::shared_ptr<ButtonDemoState> state = std::static_pointer_cast<ButtonDemoState>(*ctx->state);
    if (action->name == "increment")
    {
        (state->number)++;
        printf("%d\n", state->number);
    }

    return {};
}

static void logic_course_07(ChannelContext *ctx)
{
}

REGISTER_LOGIC(logic_course_07);
REGISTER_LOGIC_ACTION(logic_course_07, logic_course_07_action);
