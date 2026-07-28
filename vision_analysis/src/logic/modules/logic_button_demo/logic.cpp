#include "logic/core/logic_common.h"

#include <climits>
#include <memory>
#include <string>

/* 每个通道都有自己独立的数字，初始值为 1。 */
struct ButtonDemoState
{
    int number = 1;
};

/* 获取当前通道的状态；第一次调用时创建状态。 */
static ButtonDemoState *get_button_demo_state(ChannelContext *ctx)
{
    if (ctx == nullptr || ctx->state == nullptr)
        return nullptr;

    /* ctx->state 是框架规定的 shared_ptr，这里必须使用一次 C++ 智能指针。 */
    if (!*ctx->state)
        *ctx->state = std::make_shared<ButtonDemoState>();

    return static_cast<ButtonDemoState *>(ctx->state->get());
}

/* Web 点击按钮后，框架会在下一次处理该通道画面时调用这个函数。 */
static ChannelActionResult logic_button_demo_action(ChannelContext *ctx, const ChannelAction *action)
{
    ChannelActionResult result;
    if (action == nullptr)
    {
        result.message = "action is null";
        return result;
    }

    ButtonDemoState *state = get_button_demo_state(ctx);

    if (state == nullptr)
    {
        result.message = "state is null";
        return result;
    }

    if (action->name == "increment")
    {
        if (state->number < INT_MAX)
            state->number += 1;

        result.handled = true;
        result.message = "number=" + std::to_string(state->number);
        return result;
    }

    if (action->name == "decrement")
    {
        if (state->number > 1)
            state->number -= 1;

        result.handled = true;
        result.message = "number=" + std::to_string(state->number);
        return result;
    }

    result.message = "unsupported action: " + action->name;
    return result;
}

/* 每一帧都把当前数字画到视频画面中央。 */
static void logic_button_demo(ChannelContext *ctx)
{
    if (ctx == nullptr || ctx->state == nullptr || ctx->frame == nullptr || ctx->frame->empty())
        return;

    ButtonDemoState *state = get_button_demo_state(ctx);
    if (state == nullptr)
        return;

    std::string text = std::to_string(state->number);

    /* 按每个数字大约 60 像素宽进行简单居中。 */
    int text_width = static_cast<int>(text.length()) * 60;
    int x = (ctx->frame->cols - text_width) / 2;
    int y = ctx->frame->rows / 2;

    if (x < 20)
        x = 20;
    if (y < 80)
        y = 80;

    draw_text(ctx, "Web button demo: +1 / -1", cv::Point(20, 40), cv::Scalar(255, 255, 255), 0.8, 2);
    draw_text(ctx, text.c_str(), cv::Point(x, y), cv::Scalar(0, 255, 255), 3.0, 5, DrawCommand::ALL);
}

/* 注册逐帧函数和按钮处理函数。 */
REGISTER_LOGIC(logic_button_demo);
REGISTER_LOGIC_ACTION(logic_button_demo, logic_button_demo_action);
