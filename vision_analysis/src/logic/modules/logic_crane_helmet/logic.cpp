#include "logic/core/logic_common.h"

#include <cstdio>
#include <memory>
#include <string>

#include "logic/modules/logic_crane_helmet/detection_utils.h"
#include "logic/modules/logic_crane_helmet/helmet_guard.h"

namespace
{

enum HelmetSetting
{
    PERSON_MIN_SCORE = 0,
    HELMET_MIN_SCORE,
    HELMET_CONFIRM_TIME,
    HELMET_CLEAR_TIME,
    HELMET_SETTING_COUNT
};

struct HelmetLogicState
{
    crane_safety::HelmetGuard guard;
    bool controls_initialized = false;
    float person_min_score = 0.4f;
    float helmet_min_score = 0.3f;
    uint64_t confirm_ms = 500;
    uint64_t clear_ms = 1000;
    HelmetSetting selected_setting = PERSON_MIN_SCORE;
};

void draw_outlined_status(ChannelContext *ctx, const char *text, const cv::Point &pos,
                          const cv::Scalar &foreground)
{
    draw_text(ctx, text, pos, foreground, 0.7, 1, DrawCommand::ALL,
              /*shadow_enabled=*/true, cv::Scalar(15, 45, 90), 2);
}

void initialize_controls(ChannelContext *ctx, HelmetLogicState &state)
{
    if (state.controls_initialized)
        return;
    state.person_min_score = ctx->param_float("person_min_score");
    state.helmet_min_score = ctx->param_float("helmet_min_score");
    state.confirm_ms = crane_safety::seconds_to_ms(ctx->param_float("confirm_sec"));
    state.clear_ms = crane_safety::seconds_to_ms(ctx->param_float("clear_sec"));
    state.controls_initialized = true;
}

const char *helmet_setting_name(HelmetSetting setting)
{
    switch (setting)
    {
    case PERSON_MIN_SCORE:
        return "人员最低置信度";
    case HELMET_MIN_SCORE:
        return "安全帽最低置信度";
    case HELMET_CONFIRM_TIME:
        return "违规确认时间";
    case HELMET_CLEAR_TIME:
        return "告警解除时间";
    case HELMET_SETTING_COUNT:
        break;
    }
    return "未知参数";
}

void format_selected_setting(const HelmetLogicState &state, char *text, size_t size)
{
    if (state.selected_setting == HELMET_CONFIRM_TIME)
        std::snprintf(text, size, "参数: %s = %.1fs", helmet_setting_name(state.selected_setting),
                      state.confirm_ms / 1000.0);
    else if (state.selected_setting == HELMET_CLEAR_TIME)
        std::snprintf(text, size, "参数: %s = %.1fs", helmet_setting_name(state.selected_setting),
                      state.clear_ms / 1000.0);
    else
    {
        float value = state.person_min_score;
        if (state.selected_setting == HELMET_MIN_SCORE)
            value = state.helmet_min_score;
        std::snprintf(text, size, "参数: %s = %.2f", helmet_setting_name(state.selected_setting), value);
    }
}

void adjust_selected_setting(HelmetLogicState &state, bool increase)
{
    const float direction = increase ? 1.0f : -1.0f;
    switch (state.selected_setting)
    {
    case PERSON_MIN_SCORE:
        state.person_min_score = std::max(0.0f, std::min(1.0f, state.person_min_score + direction * 0.05f));
        break;
    case HELMET_MIN_SCORE:
        state.helmet_min_score = std::max(0.0f, std::min(1.0f, state.helmet_min_score + direction * 0.05f));
        break;
    case HELMET_CONFIRM_TIME:
        if (increase)
            state.confirm_ms = std::min<uint64_t>(30000U, state.confirm_ms + 300U);
        else
            state.confirm_ms = state.confirm_ms >= 300U ? state.confirm_ms - 300U : 0U;
        break;
    case HELMET_CLEAR_TIME:
        if (increase)
            state.clear_ms = std::min<uint64_t>(30000U, state.clear_ms + 300U);
        else
            state.clear_ms = state.clear_ms >= 300U ? state.clear_ms - 300U : 0U;
        break;
    case HELMET_SETTING_COUNT:
        break;
    }
}

crane_safety::HelmetConfig read_config(ChannelContext *ctx, HelmetLogicState &state)
{
    initialize_controls(ctx, state);
    crane_safety::HelmetConfig config;
    config.enabled = true;
    config.person_labels = crane_safety::split_labels(ctx->param_string("person_labels"));
    config.helmet_labels = crane_safety::split_labels(ctx->param_string("helmet_labels"));
    config.person_min_score = state.person_min_score;
    config.helmet_min_score = state.helmet_min_score;
    config.confirm_ms = state.confirm_ms;
    config.clear_ms = state.clear_ms;
    return config;
}

} // namespace

static LogicActionResult logic_crane_helmet_action(ChannelContext *ctx, const LogicAction *action)
{
    if (!ctx || !ctx->state || !action)
        return {false, "ctx or action is null"};
    if (!*ctx->state)
        *ctx->state = std::make_shared<HelmetLogicState>();
    HelmetLogicState &state = *std::static_pointer_cast<HelmetLogicState>(*ctx->state);
    initialize_controls(ctx, state);
    if (action->name == "reset_helmet_alarm")
    {
        state.guard.reset();
        return {true, "安全帽违规确认和解除状态已复位"};
    }
    if (action->name == "switch_helmet_setting")
    {
        state.selected_setting = static_cast<HelmetSetting>(
            (static_cast<int>(state.selected_setting) + 1) % static_cast<int>(HELMET_SETTING_COUNT));
        return {true, std::string("当前参数：") + helmet_setting_name(state.selected_setting)};
    }
    if (action->name == "helmet_setting_decrease" || action->name == "helmet_setting_increase")
    {
        adjust_selected_setting(state, action->name == "helmet_setting_increase");
        char message[192];
        format_selected_setting(state, message, sizeof(message));
        return {true, message};
    }
    return {false, "unsupported action: " + action->name};
}

static void logic_crane_helmet(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results)
        return;
    if (!*ctx->state)
        *ctx->state = std::make_shared<HelmetLogicState>();
    HelmetLogicState &state = *std::static_pointer_cast<HelmetLogicState>(*ctx->state);

    const std::string roi_name = ctx->param_string("roi_name");
    const RoiZone *zone = roi_name.empty() ? nullptr : ctx->roi_by_name(roi_name.c_str());
    const crane_safety::HelmetConfig config = read_config(ctx, state);
    const crane_safety::HelmetResult result =
        state.guard.update(*ctx->results, zone, ctx->timestamp_ms, config);

    ctx->publish_bool("helmet_roi_available", result.roi_available);
    ctx->publish_bool("helmet_alarm", result.alarm);
    ctx->publish_int("helmet_person_count", result.person_count);
    ctx->publish_int("unhelmeted_count", result.unhelmeted_count);

    char line1[192];
    char line2[192];
    char line3[192];
    char line4[192];
    char line5[192];
    if (!result.roi_available)
        std::snprintf(line1, sizeof(line1), "安全帽检测: 未找到ROI %s", roi_name.c_str());
    else
        std::snprintf(line1, sizeof(line1), "人员: %d  未戴: %d  告警: %s",
                      result.person_count, result.unhelmeted_count, result.alarm ? "是" : "否");
    std::snprintf(line2, sizeof(line2), "人员置信度: %.2f", state.person_min_score);
    std::snprintf(line3, sizeof(line3), "安全帽置信度: %.2f", state.helmet_min_score);
    std::snprintf(line4, sizeof(line4), "违规确认: %.1f/%.1fs",
                  result.confirm_elapsed_ms / 1000.0, state.confirm_ms / 1000.0);
    std::snprintf(line5, sizeof(line5), "告警解除: %.1f/%.1fs",
                  result.clear_elapsed_ms / 1000.0, state.clear_ms / 1000.0);
    const cv::Scalar normal_color(240, 240, 240);
    const cv::Scalar selected_color(0, 255, 255);
    draw_outlined_status(ctx, line1, cv::Point(18, 32),
                         result.alarm ? cv::Scalar(0, 0, 255) : cv::Scalar(240, 240, 240));
    draw_outlined_status(ctx, line2, cv::Point(18, 62),
                         state.selected_setting == PERSON_MIN_SCORE ? selected_color : normal_color);
    draw_outlined_status(ctx, line3, cv::Point(18, 92),
                         state.selected_setting == HELMET_MIN_SCORE ? selected_color : normal_color);
    draw_outlined_status(ctx, line4, cv::Point(18, 122),
                         state.selected_setting == HELMET_CONFIRM_TIME ? selected_color : normal_color);
    draw_outlined_status(ctx, line5, cv::Point(18, 152),
                         state.selected_setting == HELMET_CLEAR_TIME ? selected_color : normal_color);
}

REGISTER_LOGIC(logic_crane_helmet);
REGISTER_LOGIC_ACTION(logic_crane_helmet, logic_crane_helmet_action);
