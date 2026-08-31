#include "logic/core/logic_common.h"

#include <cstdio>
#include <memory>
#include <string>

#include "logic/modules/logic_crane_intrusion/detection_utils.h"
#include "logic/modules/logic_crane_intrusion/intrusion_guard.h"

namespace
{

enum IntrusionSetting
{
    INTRUSION_MIN_SCORE = 0,
    INTRUSION_CONFIRM_TIME,
    INTRUSION_CLEAR_TIME,
    INTRUSION_SETTING_COUNT
};

struct IntrusionLogicState
{
    crane_safety::IntrusionGuard guard;
    bool controls_initialized = false;
    float min_score = 0.4f;
    uint64_t confirm_ms = 300;
    uint64_t clear_ms = 1000;
    IntrusionSetting selected_setting = INTRUSION_MIN_SCORE;
};

void draw_outlined_status(ChannelContext *ctx, const char *text, const cv::Point &pos,
                          const cv::Scalar &foreground)
{
    draw_text(ctx, text, pos, foreground, 0.7, 1, DrawCommand::ALL,
              /*shadow_enabled=*/true, cv::Scalar(15, 45, 90), 2);
}

void initialize_controls(ChannelContext *ctx, IntrusionLogicState &state)
{
    if (state.controls_initialized)
        return;
    state.min_score = ctx->param_float("min_score");
    state.confirm_ms = crane_safety::seconds_to_ms(ctx->param_float("confirm_sec"));
    state.clear_ms = crane_safety::seconds_to_ms(ctx->param_float("clear_sec"));
    state.controls_initialized = true;
}

const char *intrusion_setting_name(IntrusionSetting setting)
{
    switch (setting)
    {
    case INTRUSION_MIN_SCORE:
        return "人员最低置信度";
    case INTRUSION_CONFIRM_TIME:
        return "入侵确认时间";
    case INTRUSION_CLEAR_TIME:
        return "告警解除时间";
    case INTRUSION_SETTING_COUNT:
        break;
    }
    return "未知参数";
}

void format_selected_setting(const IntrusionLogicState &state, char *text, size_t size)
{
    if (state.selected_setting == INTRUSION_MIN_SCORE)
        std::snprintf(text, size, "参数: %s = %.2f", intrusion_setting_name(state.selected_setting),
                      state.min_score);
    else
    {
        const uint64_t value = state.selected_setting == INTRUSION_CONFIRM_TIME ? state.confirm_ms : state.clear_ms;
        std::snprintf(text, size, "参数: %s = %.1fs", intrusion_setting_name(state.selected_setting),
                      value / 1000.0);
    }
}

void adjust_selected_setting(IntrusionLogicState &state, bool increase)
{
    if (state.selected_setting == INTRUSION_MIN_SCORE)
    {
        state.min_score = std::max(0.0f, std::min(1.0f, state.min_score + (increase ? 0.05f : -0.05f)));
        return;
    }
    uint64_t &value = state.selected_setting == INTRUSION_CONFIRM_TIME ? state.confirm_ms : state.clear_ms;
    if (increase)
        value = std::min<uint64_t>(30000U, value + 300U);
    else
        value = value >= 300U ? value - 300U : 0U;
}

crane_safety::IntrusionConfig read_config(ChannelContext *ctx, IntrusionLogicState &state)
{
    initialize_controls(ctx, state);
    crane_safety::IntrusionConfig config;
    config.enabled = true;
    config.person_labels = crane_safety::split_labels(ctx->param_string("person_labels"));
    config.min_score = state.min_score;
    config.confirm_ms = state.confirm_ms;
    config.clear_ms = state.clear_ms;
    return config;
}

} // namespace

static LogicActionResult logic_crane_intrusion_action(ChannelContext *ctx, const LogicAction *action)
{
    if (!ctx || !ctx->state || !action)
        return {false, "ctx or action is null"};
    if (!*ctx->state)
        *ctx->state = std::make_shared<IntrusionLogicState>();
    IntrusionLogicState &state = *std::static_pointer_cast<IntrusionLogicState>(*ctx->state);
    initialize_controls(ctx, state);
    if (action->name == "reset_intrusion_alarm")
    {
        state.guard.reset();
        return {true, "人员入侵确认和解除状态已复位"};
    }
    if (action->name == "switch_intrusion_setting")
    {
        state.selected_setting = static_cast<IntrusionSetting>(
            (static_cast<int>(state.selected_setting) + 1) % static_cast<int>(INTRUSION_SETTING_COUNT));
        return {true, std::string("当前参数：") + intrusion_setting_name(state.selected_setting)};
    }
    if (action->name == "intrusion_setting_decrease" || action->name == "intrusion_setting_increase")
    {
        adjust_selected_setting(state, action->name == "intrusion_setting_increase");
        char message[192];
        format_selected_setting(state, message, sizeof(message));
        return {true, message};
    }
    return {false, "unsupported action: " + action->name};
}

static void logic_crane_intrusion(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results)
        return;
    if (!*ctx->state)
        *ctx->state = std::make_shared<IntrusionLogicState>();
    IntrusionLogicState &state = *std::static_pointer_cast<IntrusionLogicState>(*ctx->state);

    const std::string roi_name = ctx->param_string("roi_name");
    const RoiZone *zone = roi_name.empty() ? nullptr : ctx->roi_by_name(roi_name.c_str());
    const crane_safety::IntrusionConfig config = read_config(ctx, state);
    const crane_safety::IntrusionResult result =
        state.guard.update(*ctx->results, zone, ctx->timestamp_ms, config);

    ctx->publish_bool("intrusion_roi_available", result.roi_available);
    ctx->publish_bool("intrusion_alarm", result.alarm);
    ctx->publish_int("intrusion_person_count", result.person_count);

    char line1[192];
    char line2[192];
    char line3[192];
    char line4[192];
    if (!result.roi_available)
        std::snprintf(line1, sizeof(line1), "入侵检测: 未找到ROI %s", roi_name.c_str());
    else
        std::snprintf(line1, sizeof(line1), "区域人员: %d  告警: %s",
                      result.person_count, result.alarm ? "是" : "否");
    std::snprintf(line2, sizeof(line2), "人员置信度: %.2f", state.min_score);
    std::snprintf(line3, sizeof(line3), "入侵确认: %.1f/%.1fs",
                  result.confirm_elapsed_ms / 1000.0, state.confirm_ms / 1000.0);
    std::snprintf(line4, sizeof(line4), "告警解除: %.1f/%.1fs",
                  result.clear_elapsed_ms / 1000.0, state.clear_ms / 1000.0);
    const cv::Scalar normal_color(240, 240, 240);
    const cv::Scalar selected_color(0, 255, 255);
    draw_outlined_status(ctx, line1, cv::Point(18, 32),
                         result.alarm ? cv::Scalar(0, 0, 255) : cv::Scalar(240, 240, 240));
    draw_outlined_status(ctx, line2, cv::Point(18, 62),
                         state.selected_setting == INTRUSION_MIN_SCORE ? selected_color : normal_color);
    draw_outlined_status(ctx, line3, cv::Point(18, 92),
                         state.selected_setting == INTRUSION_CONFIRM_TIME ? selected_color : normal_color);
    draw_outlined_status(ctx, line4, cv::Point(18, 122),
                         state.selected_setting == INTRUSION_CLEAR_TIME ? selected_color : normal_color);
}

REGISTER_LOGIC(logic_crane_intrusion);
REGISTER_LOGIC_ACTION(logic_crane_intrusion, logic_crane_intrusion_action);
