// 课程7:如何添加一个带自定义功能的按钮
// 实现效果：3个按钮，按钮1控制数字+1，按钮2控制数字-1，按钮3切换控制的数字
// 按钮控制算法逻辑的本质是什么？按下按钮后，算法逻辑是怎么接收到信号的
// 难度:★★★☆☆

#include "logic/core/logic_common.h"

// 跨帧状态
struct ButtonDemoState
{
    int num = 0;
    int number[2] = {0, 0};
    std::vector<cv::Point> num_loc = {cv::Point(500, 65), cv::Point(500, 130)};
};

// 返回类型不能是 void，所有执行路径都必须返回 ChannelActionResult，否则属于未定义行为
static ChannelActionResult logic_course_07_action(ChannelContext *ctx, const ChannelAction *action)
{
    if (!ctx || !ctx->state || !action)
    {
        return {false, "ctx or action is null"};
    }
    ChannelActionResult result;
    if (!*(ctx->state))
    {
        // 第一次按按钮的时候,初始化状态
        *(ctx->state) = std::make_shared<ButtonDemoState>();
    }
    std::shared_ptr<ButtonDemoState> state = std::static_pointer_cast<ButtonDemoState>(*ctx->state);
    // 数字+1按钮
    if (action->name == "increment")
    {
        (state->number[state->num])++;
        result.handled = true;
        result.message = "number加1";
        return result;
    }
    // 数字-1按钮
    if (action->name == "decrement")
    {
        (state->number[state->num])--;
        result.handled = true;
        result.message = "number减1";
        return result;
    }
    // 切换数字
    if (action->name == "change")
    {
        state->num = !state->num;
        result.handled = true;
        result.message = "切换数字";
        return result;
    }
    result.handled = false;
    result.message = "unsupported action: " + action->name;
    return result;
}

static void logic_course_07(ChannelContext *ctx)
{
    if (!ctx || !ctx->state)
    {
        return;
    }
    // 即使没有按过按钮，通道逻辑也需要初始化状态
    if (!*ctx->state)
    {
        *ctx->state = std::make_shared<ButtonDemoState>();
    }
    std::shared_ptr<ButtonDemoState> state = std::static_pointer_cast<ButtonDemoState>(*ctx->state);
    char str1[32];
    char str2[32];
    snprintf(str1, sizeof(str1), "%d", state->number[0]);
    snprintf(str2, sizeof(str2), "%d", state->number[1]);

    draw_text(ctx, str1, state->num_loc[0], (state->num == 0) ? cv::Scalar(0, 255, 255) : cv::Scalar(255, 255, 255),
              1.8, 3, DrawCommand::ALL);
    draw_text(ctx, str2, state->num_loc[1], (state->num == 1) ? cv::Scalar(0, 255, 255) : cv::Scalar(255, 255, 255),
              1.8, 3, DrawCommand::ALL);
}

// 注册主通道逻辑
REGISTER_LOGIC(logic_course_07);

// 将按钮的逻辑注册到主通道逻辑中
REGISTER_LOGIC_ACTION(logic_course_07, logic_course_07_action);

/*
    把以下3个命令中的RK3588的设备 IP、App 名称和通道 ID
   替换为实际值后在与RK3588局域网的任何一台能使用curl命令的设备上运行, 看视频画面会发生什么

    App 名称是程序安装在 /opt/ai_apps/ 下的目录名称，
    也就是 Web 控制台“程序管理”页面显示的应用名称。

    通道 ID 是配置文件 config.channels[].id，不是 channels 数组下标。

    Linux、macOS Bash、Windows PowerShell：

    1. curl -X POST "http://设备IP:8080/api/apps/App名称/channels/通道ID/actions/increment" -H "Content-Type:
   application/json" -d '{"payload":{}}'

    2. curl -X POST "http://设备IP:8080/api/apps/App名称/channels/通道ID/actions/decrement" -H "Content-Type:
   application/json" -d '{"payload":{}}'

    3. curl -X POST "http://设备IP:8080/api/apps/App名称/channels/通道ID/actions/change" -H "Content-Type:
   application/json" -d '{"payload":{}}'

    示例：

    curl -X POST "http://192.168.2.43:8080/api/apps/test_7_27/channels/0/actions/change" -H "Content-Type:
   application/json" -d '{"payload":{}}'

    Windows CMD 需要使用 curl.exe，并对 JSON 中的双引号进行转义：

    curl.exe -X POST "http://设备IP:8080/api/apps/App名称/channels/通道ID/actions/change" -H "Content-Type:
   application/json" -d "{\"payload\":{}}"

    注意：
    1. 应用必须处于运行状态。
    2. 指定通道当前必须使用 logic_course_07。
    3. HTTP 返回 accepted 只表示动作已经进入队列，不代表处理函数已经执行完成。
    4. 当前通道 Action 接口已配置为免 Token，局域网中任何能访问8080端口的设备不需要RK3588的用户名和密码也能触发。
*/
