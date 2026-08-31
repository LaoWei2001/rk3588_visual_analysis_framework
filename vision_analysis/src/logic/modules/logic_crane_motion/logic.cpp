#include "logic/core/logic_common.h"

#include <cstdio>
#include <memory>
#include <string>

#include "logic/modules/logic_crane_motion/detection_utils.h"
#include "logic/modules/logic_crane_motion/motion_detector.h"

namespace
{

enum MotionSetting
{
    DIFF_THRESHOLD = 0,
    START_CHANGE_RATIO,
    STOP_CHANGE_RATIO,
    MOVING_CONFIRM_TIME,
    STILL_CONFIRM_TIME,
    MOTION_SETTING_COUNT
};

struct MotionLogicState
{
    crane_safety::MotionDetector detector;
    bool controls_initialized = false;
    int diff_threshold = 25;
    float start_ratio = 0.18f;
    float stop_ratio = 0.06f;
    uint64_t moving_confirm_ms = 400;
    uint64_t still_confirm_ms = 2000;
    MotionSetting selected_setting = DIFF_THRESHOLD;
};

void draw_outlined_status(ChannelContext *ctx, const char *text, const cv::Point &pos,
                          const cv::Scalar &foreground)
{
    draw_text(ctx, text, pos, foreground, 0.7, 1, DrawCommand::ALL,
              /*shadow_enabled=*/true, cv::Scalar(15, 45, 90), 2);
}

void visualize_changed_pixels(ChannelContext *ctx, const cv::Mat &change_mask, const cv::Size &frame_size)
{
    if (!ctx || change_mask.empty() || frame_size.width <= 0 || frame_size.height <= 0)
        return;

    /* 检测仍是逐像素帧差；可视化统一交给底层 SIMD/NEON 蒙版融合。 */
    blend_display_mask(ctx, change_mask, cv::Scalar(0, 140, 255), 0.4);
}

void initialize_controls(ChannelContext *ctx, MotionLogicState &state)
{
    if (state.controls_initialized)
        return;
    state.diff_threshold = static_cast<int>(ctx->param_int("diff_threshold"));
    state.start_ratio = ctx->param_float("start_ratio");
    state.stop_ratio = ctx->param_float("stop_ratio");
    state.moving_confirm_ms = crane_safety::seconds_to_ms(ctx->param_float("moving_confirm_sec"));
    state.still_confirm_ms = crane_safety::seconds_to_ms(ctx->param_float("still_confirm_sec"));
    state.controls_initialized = true;
}

const char *motion_setting_name(MotionSetting setting)
{
    switch (setting)
    {
    case DIFF_THRESHOLD:
        return "帧差灰度阈值";
    case START_CHANGE_RATIO:
        return "运动变化比例";
    case STOP_CHANGE_RATIO:
        return "静止变化比例";
    case MOVING_CONFIRM_TIME:
        return "运动确认时间";
    case STILL_CONFIRM_TIME:
        return "静止确认时间";
    case MOTION_SETTING_COUNT:
        break;
    }
    return "未知参数";
}

void format_selected_setting(const MotionLogicState &state, char *text, size_t size)
{
    switch (state.selected_setting)
    {
    case DIFF_THRESHOLD:
        std::snprintf(text, size, "参数: %s = %d", motion_setting_name(state.selected_setting),
                      state.diff_threshold);
        break;
    case START_CHANGE_RATIO:
        std::snprintf(text, size, "参数: %s = %.1f%%", motion_setting_name(state.selected_setting),
                      state.start_ratio * 100.0f);
        break;
    case STOP_CHANGE_RATIO:
        std::snprintf(text, size, "参数: %s = %.1f%%", motion_setting_name(state.selected_setting),
                      state.stop_ratio * 100.0f);
        break;
    case MOVING_CONFIRM_TIME:
        std::snprintf(text, size, "参数: %s = %.1fs", motion_setting_name(state.selected_setting),
                      state.moving_confirm_ms / 1000.0);
        break;
    case STILL_CONFIRM_TIME:
        std::snprintf(text, size, "参数: %s = %.1fs", motion_setting_name(state.selected_setting),
                      state.still_confirm_ms / 1000.0);
        break;
    case MOTION_SETTING_COUNT:
        std::snprintf(text, size, "参数: 未知");
        break;
    }
}

void adjust_selected_setting(MotionLogicState &state, bool increase)
{
    const float direction = increase ? 1.0f : -1.0f;
    switch (state.selected_setting)
    {
    case DIFF_THRESHOLD:
        state.diff_threshold = std::max(1, std::min(255, state.diff_threshold + (increase ? 5 : -5)));
        break;
    case START_CHANGE_RATIO:
        state.start_ratio = std::max(state.stop_ratio, std::min(1.0f, state.start_ratio + direction * 0.01f));
        break;
    case STOP_CHANGE_RATIO:
        state.stop_ratio = std::max(0.0f, std::min(state.start_ratio, state.stop_ratio + direction * 0.01f));
        break;
    case MOVING_CONFIRM_TIME:
        if (increase)
            state.moving_confirm_ms = std::min<uint64_t>(10000U, state.moving_confirm_ms + 300U);
        else
            state.moving_confirm_ms = state.moving_confirm_ms >= 300U ? state.moving_confirm_ms - 300U : 0U;
        break;
    case STILL_CONFIRM_TIME:
        if (increase)
            state.still_confirm_ms = std::min<uint64_t>(60000U, state.still_confirm_ms + 300U);
        else
            state.still_confirm_ms = state.still_confirm_ms >= 300U ? state.still_confirm_ms - 300U : 0U;
        break;
    case MOTION_SETTING_COUNT:
        break;
    }
}

crane_safety::MotionConfig read_config(ChannelContext *ctx, MotionLogicState &state)
{
    initialize_controls(ctx, state);
    crane_safety::MotionConfig config;
    config.diff_threshold = state.diff_threshold;
    /* 预处理固定使用 MotionConfig 的默认值：5x5 高斯核并开启亮度归一化。
     * 这两项不再暴露给现场配置，避免误调后改变运动判断基础。 */
    config.start_ratio = state.start_ratio;
    config.stop_ratio = state.stop_ratio;
    config.moving_confirm_ms = state.moving_confirm_ms;
    config.still_confirm_ms = state.still_confirm_ms;
    return config;
}

} // namespace

static LogicActionResult logic_crane_motion_action(ChannelContext *ctx, const LogicAction *action)
{
    if (!ctx || !ctx->state || !action)
        return {false, "ctx or action is null"};
    if (!*ctx->state)
        *ctx->state = std::make_shared<MotionLogicState>();
    MotionLogicState &state = *std::static_pointer_cast<MotionLogicState>(*ctx->state);
    initialize_controls(ctx, state);
    if (action->name == "switch_motion_setting")
    {
        state.selected_setting = static_cast<MotionSetting>(
            (static_cast<int>(state.selected_setting) + 1) % static_cast<int>(MOTION_SETTING_COUNT));
        return {true, std::string("当前参数：") + motion_setting_name(state.selected_setting)};
    }
    if (action->name == "motion_setting_decrease" || action->name == "motion_setting_increase")
    {
        adjust_selected_setting(state, action->name == "motion_setting_increase");
        char message[192];
        format_selected_setting(state, message, sizeof(message));
        return {true, message};
    }
    return {false, "unsupported action: " + action->name};
}

static void logic_crane_motion(ChannelContext *ctx)
{
    if (!ctx || !ctx->state)
        return;
    const cv::Mat *frame = ctx->model_frame();
    if (!frame || frame->empty())
        return;
    if (!*ctx->state)
        *ctx->state = std::make_shared<MotionLogicState>();
    MotionLogicState &state = *std::static_pointer_cast<MotionLogicState>(*ctx->state);

    const std::string roi_name = ctx->param_string("motion_roi_name");
    const RoiZone *zone = roi_name.empty() ? nullptr : ctx->roi_by_name(roi_name.c_str());
    const crane_safety::MotionResult result =
        state.detector.update(*frame, zone, ctx->timestamp_ms, read_config(ctx, state));

    ctx->publish_bool("motion_valid", result.initialized);
    ctx->publish_bool("crane_moving", result.moving);
    ctx->publish_number("motion_change_ratio", result.change_ratio);
    visualize_changed_pixels(ctx, result.change_mask, frame->size());

    char line1[192];
    char line2[192];
    char line3[192];
    char line4[192];
    char line5[192];
    char line6[192];
    std::snprintf(line1, sizeof(line1), "行车: %s  变化: %.2f%%",
                  result.moving ? "运动" : "静止", result.change_ratio * 100.0f);
    std::snprintf(line2, sizeof(line2), "帧差: %d", state.diff_threshold);
    std::snprintf(line3, sizeof(line3), "运动比例: >= %.1f%%", state.start_ratio * 100.0f);
    std::snprintf(line4, sizeof(line4), "静止比例: <= %.1f%%", state.stop_ratio * 100.0f);
    std::snprintf(line5, sizeof(line5), "运动确认: %.1f/%.1fs",
                  result.moving_candidate_elapsed_ms / 1000.0, state.moving_confirm_ms / 1000.0);
    std::snprintf(line6, sizeof(line6), "静止确认: %.1f/%.1fs",
                  result.still_candidate_elapsed_ms / 1000.0, state.still_confirm_ms / 1000.0);
    const cv::Scalar normal_color(240, 240, 240);
    const cv::Scalar selected_color(0, 255, 255);
    draw_outlined_status(ctx, line1, cv::Point(18, 32),
                         result.moving ? cv::Scalar(0, 165, 255) : cv::Scalar(0, 220, 0));
    draw_outlined_status(ctx, line2, cv::Point(18, 62),
                         state.selected_setting == DIFF_THRESHOLD ? selected_color : normal_color);
    draw_outlined_status(ctx, line3, cv::Point(18, 92),
                         state.selected_setting == START_CHANGE_RATIO ? selected_color : normal_color);
    draw_outlined_status(ctx, line4, cv::Point(18, 122),
                         state.selected_setting == STOP_CHANGE_RATIO ? selected_color : normal_color);
    draw_outlined_status(ctx, line5, cv::Point(18, 152),
                         state.selected_setting == MOVING_CONFIRM_TIME ? selected_color : normal_color);
    draw_outlined_status(ctx, line6, cv::Point(18, 182),
                         state.selected_setting == STILL_CONFIRM_TIME ? selected_color : normal_color);
}

REGISTER_LOGIC(logic_crane_motion);
REGISTER_LOGIC_ACTION(logic_crane_motion, logic_crane_motion_action);
