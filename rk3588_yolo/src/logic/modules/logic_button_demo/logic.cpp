#include "logic/core/logic_common.h"

#include <memory>
#include <string>

struct ButtonDemoState
{
    int press_count = 0;
    int alarm_count = 0;
    bool hold_enabled = false;
    bool pending_alarm = false;
    uint64_t pulse_until_ms = 0;
    uint64_t last_action_mono_ms = 0;
    std::string last_action = "none";
    std::string last_message = "waiting web action";
    std::string last_action_time = "--:--:--";
};

static ButtonDemoState &button_demo_state(ChannelContext *ctx)
{
    if (!*ctx->state)
        *ctx->state = std::make_shared<ButtonDemoState>();
    return *std::static_pointer_cast<ButtonDemoState>(*ctx->state);
}

static uint64_t button_demo_now_ms(const ChannelContext *ctx)
{
    return ctx ? ctx->timestamp_ms : 0;
}

static uint64_t button_demo_pulse_ms(const ChannelContext *ctx)
{
    const float seconds = ctx ? ctx->param_float("pulse_duration_sec") : 0.0f;
    return static_cast<uint64_t>(std::max(1.0, std::round(seconds * 1000.0)));
}

static void button_demo_mark_action(ChannelContext *ctx, ButtonDemoState &state, const std::string &action_name,
                                    const std::string &message)
{
    state.press_count += 1;
    state.last_action = action_name;
    state.last_message = message;
    state.last_action_mono_ms = button_demo_now_ms(ctx);
    state.last_action_time = ctx ? ctx->time_hms() : "--:--:--";
}

static ChannelActionResult logic_button_demo_action(ChannelContext *ctx, const ChannelAction &action)
{
    ChannelActionResult result;
    if (!ctx || !ctx->state)
    {
        result.message = "ctx is null";
        return result;
    }

    ButtonDemoState &state = button_demo_state(ctx);

    if (action.name == "pulse_overlay")
    {
        const float pulse_seconds = ctx->param_float("pulse_duration_sec");
        button_demo_mark_action(
            ctx, state, action.name,
            "flash overlay for " + std::to_string(pulse_seconds) + " seconds");
        state.pulse_until_ms = button_demo_now_ms(ctx) + button_demo_pulse_ms(ctx);
        result.handled = true;
        result.message = "pulse overlay armed";
        return result;
    }

    if (action.name == "toggle_hold")
    {
        state.hold_enabled = !state.hold_enabled;
        button_demo_mark_action(ctx, state, action.name,
                                state.hold_enabled ? "hold overlay enabled" : "hold overlay disabled");
        result.handled = true;
        result.message = state.hold_enabled ? "hold overlay enabled" : "hold overlay disabled";
        return result;
    }

    if (action.name == "trigger_alarm")
    {
        button_demo_mark_action(ctx, state, action.name, "manual alarm requested");
        state.pending_alarm = true;
        state.pulse_until_ms = button_demo_now_ms(ctx) + button_demo_pulse_ms(ctx);
        result.handled = true;
        result.message = "manual alarm queued";
        return result;
    }

    if (action.name == "clear_state")
    {
        state = ButtonDemoState();
        state.last_action = action.name;
        state.last_message = "state cleared";
        state.last_action_time = ctx->time_hms();
        state.last_action_mono_ms = button_demo_now_ms(ctx);
        state.press_count = 1;
        result.handled = true;
        result.message = "button demo state cleared";
        return result;
    }

    result.message = std::string("unsupported action: ") + action.name;
    return result;
}

static void logic_button_demo(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->frame)
        return;

    ButtonDemoState &state = button_demo_state(ctx);
    const int frame_w = ctx->frame->cols > 0 ? ctx->frame->cols : 640;
    const int frame_h = ctx->frame->rows > 0 ? ctx->frame->rows : 640;
    const bool pulse_active = state.pulse_until_ms > button_demo_now_ms(ctx);

    draw_text(ctx, "button demo logic", cv::Point(20, 30), cv::Scalar(0, 255, 255), 0.75, 2);

    draw_text(ctx, ("last action: " + state.last_action + " @ " + state.last_action_time).c_str(), cv::Point(20, 62),
              cv::Scalar(255, 255, 255), 0.58, 1);
    draw_text(ctx, ("message: " + state.last_message).c_str(), cv::Point(20, 90), cv::Scalar(255, 255, 255), 0.58, 1);
    draw_text(ctx,
              ("press_count=" + std::to_string(state.press_count) + " alarm_count=" +
               std::to_string(state.alarm_count) + " hold=" + std::string(state.hold_enabled ? "on" : "off"))
                  .c_str(),
              cv::Point(20, 118), cv::Scalar(180, 255, 180), 0.58, 1);

    if (state.hold_enabled)
    {
        draw_rect(ctx, cv::Rect(10, 10, std::max(0, frame_w - 20), std::max(0, frame_h - 20)), cv::Scalar(0, 255, 0), 3,
                  1.0, DrawCommand::DISPLAY);
        draw_text(ctx, "hold overlay is ON", cv::Point(20, 150), cv::Scalar(0, 255, 0), 0.65, 2);
    }

    if (pulse_active)
    {
        draw_rect(ctx, cv::Rect(18, 18, std::max(0, frame_w - 36), std::max(0, frame_h - 36)), cv::Scalar(0, 0, 255), 6,
                  1.0, DrawCommand::DISPLAY);
        draw_text(ctx, "button pulse active", cv::Point(20, 182), cv::Scalar(0, 0, 255), 0.8, 2);
    }

    if (state.pending_alarm)
    {
        const std::string event_id = report_alarm(ctx, "button_demo_manual", "Web button triggered demo alarm",
                                                  {
                                                      alarm_field("last_action", state.last_action),
                                                      alarm_field("press_count", state.press_count),
                                                      alarm_field("alarm_count", state.alarm_count + 1),
                                                      alarm_field("hold_enabled", state.hold_enabled),
                                                  });
        if (!event_id.empty())
        {
            state.alarm_count += 1;
            state.last_message = "manual alarm created: " + event_id;
        }
        else
        {
            state.last_message = "manual alarm ignored (no delivery or creation failed)";
        }
        state.pending_alarm = false;
    }
}

REGISTER_LOGIC(logic_button_demo);
REGISTER_LOGIC_ACTION(logic_button_demo, logic_button_demo_action);
