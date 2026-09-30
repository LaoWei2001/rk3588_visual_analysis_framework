// 继电器控制: 1 个按钮切换开启/关闭
// 实现效果: Web 控制台该通道页面上出现"开/关继电器"按钮, 点击切换 GPIO 引脚电平,
//           画面上同步显示继电器当前状态
// 难度: ★★☆☆☆
// 知识点:
//   1. REGISTER_LOGIC_ACTION 注册按钮动作, 动作函数与逐帧逻辑共享 ctx->state
//   2. 动作函数在下一帧 logic 调用前执行, ctx 与逐帧逻辑完全一致(可读参数/状态)
//   3. action->payload_json 携带按钮/HTTP 请求的 JSON 参数, 可支持更丰富的控制
//
// 接线说明: 继电器模块 IN 脚接 relay_pin 引脚(默认 GPIO6_A2), 与控制板共地。

#include "logic/core/logic_common.h"
#include "gpio/gpio.h"
#include "cJSON.h"

struct RelayState
{
    int level = -1; /* 当前实际写入的电平, -1 表示尚未写入 */
};

/* 把电平写到引脚并更新状态; 失败保持旧值, 下次调用自动重试 */
static bool relay_apply(ChannelContext *ctx, const std::shared_ptr<RelayState> &state, int level)
{
    std::string pin = ctx->param_string("relay_pin");
    if (pin.empty())
        return false;
    if (pin_out_val(pin.c_str(), level) != 0)
        return false;
    state->level = level;
    return true;
}

static LogicActionResult logic_relay_action(ChannelContext *ctx, const LogicAction *action)
{
    LogicActionResult result;
    if (!ctx || !ctx->state || !action)
    {
        result.handled = false;
        result.message = "ctx or action is null";
        return result;
    }
    if (!*ctx->state)
        *ctx->state = std::make_shared<RelayState>();
    std::shared_ptr<RelayState> state = std::static_pointer_cast<RelayState>(*ctx->state);

    int on_level = (int)ctx->param_int("on_level");
    int off_level = 1 - on_level;

    /* 目标电平: payload 带 "on" 字段则直接指定(供外部 HTTP 精确控制), 否则翻转 */
    int want = -1;
    cJSON *root = cJSON_Parse(action->payload_json.c_str());
    if (root)
    {
        cJSON *on_item = cJSON_GetObjectItemCaseSensitive(root, "on");
        if (cJSON_IsBool(on_item) || cJSON_IsNumber(on_item))
            want = on_item->valueint ? on_level : off_level;
        cJSON_Delete(root);
    }
    if (want < 0)
    {
        /* 翻转: 当前开启 -> 关, 当前关闭 -> 开 */
        bool is_on = (state->level == on_level);
        want = is_on ? off_level : on_level;
    }

    if (!relay_apply(ctx, state, want))
    {
        result.handled = true;
        result.message = "写入引脚失败(请检查引脚名/接线), 后续调用会自动重试";
        return result;
    }

    result.handled = true;
    result.message = (want == on_level) ? "继电器已开启" : "继电器已关闭";
    return result;
}

static void logic_relay(ChannelContext *ctx)
{
    if (!ctx || !ctx->state)
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<RelayState>();
    std::shared_ptr<RelayState> state = std::static_pointer_cast<RelayState>(*ctx->state);

    /* 首次进入: 按上电默认状态初始化引脚, 之后交给按钮/外部动作控制 */
    if (state->level < 0)
    {
        int on_level = (int)ctx->param_int("on_level");
        int start_on = ctx->param_bool("start_state") ? 1 : 0;
        relay_apply(ctx, state, start_on ? on_level : 1 - on_level);
    }

    /* 画面上显示当前继电器状态 */
    bool is_on = (state->level == (int)ctx->param_int("on_level"));
    draw_text(ctx, is_on ? "继电器: 开启" : "继电器: 关闭", cv::Point(30, 50),
              is_on ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 165, 255), 1.2, 2);
    std::string pin = ctx->param_string("relay_pin");
    char pin_info[128];
    snprintf(pin_info, sizeof(pin_info), "引脚: %s", pin.c_str());
    draw_text(ctx, pin_info, cv::Point(30, 85), cv::Scalar(200, 200, 200));
}

REGISTER_LOGIC(logic_relay);

REGISTER_LOGIC_ACTION(logic_relay, logic_relay_action);

/*
    Web 控制台: 把某通道的 logic 设为 logic_relay 后, 通道页面上会出现
    "开/关继电器"按钮, 点击即切换。

    也可以用 curl 直接触发(把设备 IP、App 名称、通道 ID 替换为实际值):

    1. 翻转开关(和点按钮等效):
       curl -X POST "http://设备IP:8080/api/apps/App名称/channels/通道ID/actions/toggle" \
            -H "Content-Type: application/json" -d '{"payload":{}}'

    2. 直接指定开启/关闭(供外部系统精确控制, 不走翻转):
       curl -X POST "http://设备IP:8080/api/apps/App名称/channels/通道ID/actions/toggle" \
            -H "Content-Type: application/json" -d '{"payload":{"on":true}}'
       curl -X POST "http://设备IP:8080/api/apps/App名称/channels/通道ID/actions/toggle" \
            -H "Content-Type: application/json" -d '{"payload":{"on":false}}'

    注意:
    1. 应用必须处于运行状态, 且指定通道当前使用 logic_relay。
    2. HTTP 返回 accepted 只表示动作已入队, 在下一帧 logic 调用前才真正执行。
*/
