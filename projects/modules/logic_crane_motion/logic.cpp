#include <channel.h>
#include <drawing.h>
#include <algorithm>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "detection_utils.h"
#include "motion_detector.h"

namespace
{

enum MotionSetting
{
    DIFF_THRESHOLD = 0,
    CHANGE_RATIO,
    MOVING_CONFIRM_TIME,
    STILL_CONFIRM_TIME,
    MOTION_SETTING_COUNT
};

struct MotionLogicState
{
    crane_safety::MotionDetector detector;
    bool controls_initialized = false;
    int diff_threshold = 10;
    float change_ratio = 0.015f;
    uint64_t moving_confirm_ms = 200;
    uint64_t still_confirm_ms = 2000;
    int last_hook_track_id = -1;
    MotionSetting selected_setting = DIFF_THRESHOLD;
};

const AlgoResult *select_hook_target(const std::vector<AlgoResult> *results, int class_id,
                                     float min_score, int last_track_id)
{
    if (!results)
        return nullptr;
    const AlgoResult *same_track = nullptr;
    const AlgoResult *highest_score = nullptr;
    for (const AlgoResult &result : *results)
    {
        if (result.class_id != class_id || result.score < min_score)
            continue;
        if (last_track_id >= 0 && result.track_id == last_track_id)
            same_track = &result;
        if (!highest_score || result.score > highest_score->score)
            highest_score = &result;
    }
    return same_track ? same_track : highest_score;
}

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
    state.change_ratio = ctx->param_float("change_ratio");
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
    case CHANGE_RATIO:
        return "运动/静止变化比例";
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
    case CHANGE_RATIO:
        std::snprintf(text, size, "参数: %s = %.1f%%", motion_setting_name(state.selected_setting),
                      state.change_ratio * 100.0f);
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
    case CHANGE_RATIO:
        state.change_ratio = std::max(0.001f, std::min(1.0f, state.change_ratio + direction * 0.001f));
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
    config.change_ratio = state.change_ratio;
    config.moving_confirm_ms = state.moving_confirm_ms;
    config.still_confirm_ms = state.still_confirm_ms;
    config.hook_motion_threshold_px = ctx->param_float("hook_motion_threshold_px");
    config.hook_motion_window_ms =
        crane_safety::seconds_to_ms(ctx->param_float("hook_motion_window_sec"));
    config.hook_still_reset_ms =
        crane_safety::seconds_to_ms(ctx->param_float("hook_still_reset_sec"));
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
    const AlgoResult *hook = select_hook_target(
        ctx->results, static_cast<int>(ctx->param_int("hook_class_id")),
        ctx->param_float("hook_min_score"), state.last_hook_track_id);
    cv::Point hook_center;
    if (hook)
    {
        hook_center = hook->box_center();
        state.last_hook_track_id = hook->track_id;
    }
    const crane_safety::MotionResult result =
        state.detector.update(*frame, zone, ctx->timestamp_ms, read_config(ctx, state),
                              hook ? &hook_center : nullptr, hook ? hook->track_id : -1);

    ctx->publish_bool("motion_valid", result.initialized);
    ctx->publish_bool("crane_moving", result.moving);
    ctx->publish_number("motion_change_ratio", result.change_ratio);
    ctx->publish_bool("motion_hook_visible", result.hook_visible);
    ctx->publish_bool("motion_hook_moving", result.hook_moving);
    ctx->publish_bool("motion_hook_motion_latched", result.hook_motion_latched);
    ctx->publish_number("motion_hook_displacement", result.hook_displacement);
    ctx->publish_number("motion_hook_still_reset_elapsed",
                        result.hook_still_reset_elapsed_ms / 1000.0);
    visualize_changed_pixels(ctx, result.change_mask, frame->size());

    char line1[192];
    char line2[192];
    char line3[192];
    char line4[192];
    char line5[192];
    char line6[192];
    std::snprintf(line1, sizeof(line1), "行车: %s", result.moving ? "运动" : "静止");
    std::snprintf(line2, sizeof(line2), "帧差: %d", state.diff_threshold);
    std::snprintf(line3, sizeof(line3), "背景变化比例: %.2f%% / %.1f%%", result.change_ratio * 100.0f,
                  state.change_ratio * 100.0f);
    if (result.background_motion_detected && !result.hook_motion_latched)
        std::snprintf(line4, sizeof(line4), "运动确认: 等待吊钩移动 (0.0/%.1fs)",
                      state.moving_confirm_ms / 1000.0);
    else
        std::snprintf(line4, sizeof(line4), "运动确认: %.1f/%.1fs",
                      result.moving_candidate_elapsed_ms / 1000.0,
                      state.moving_confirm_ms / 1000.0);
    std::snprintf(line5, sizeof(line5), "静止确认: %.1f/%.1fs",
                  result.still_candidate_elapsed_ms / 1000.0, state.still_confirm_ms / 1000.0);
    const double hook_reset_sec = ctx->param_float("hook_still_reset_sec");
    if (result.hook_visible && result.hook_moving)
        std::snprintf(line6, sizeof(line6), "吊钩门控: 已移动并放行  %.1f/%.1fpx",
                      result.hook_displacement, ctx->param_float("hook_motion_threshold_px"));
    else if (result.hook_visible && result.hook_motion_latched)
        std::snprintf(line6, sizeof(line6), "吊钩门控: 静止复位 %.1f/%.1fs",
                      result.hook_still_reset_elapsed_ms / 1000.0, hook_reset_sec);
    else if (!result.hook_visible && result.hook_motion_latched)
        std::snprintf(line6, sizeof(line6), "吊钩门控: 检测丢失，保持放行");
    else if (result.hook_visible)
        std::snprintf(line6, sizeof(line6), "吊钩门控: 静止，禁止运动  %.1f/%.1fpx",
                      result.hook_displacement, ctx->param_float("hook_motion_threshold_px"));
    else
        std::snprintf(line6, sizeof(line6), "吊钩门控: 未检测到，禁止运动");
    const cv::Scalar normal_color(240, 240, 240);
    const cv::Scalar selected_color(0, 255, 255);
    draw_outlined_status(ctx, line1, cv::Point(18, 32),
                         result.moving ? cv::Scalar(0, 165, 255) : cv::Scalar(0, 220, 0));
    draw_outlined_status(ctx, line2, cv::Point(18, 62),
                         state.selected_setting == DIFF_THRESHOLD ? selected_color : normal_color);
    draw_outlined_status(ctx, line3, cv::Point(18, 92),
                         state.selected_setting == CHANGE_RATIO ? selected_color : normal_color);
    draw_outlined_status(ctx, line4, cv::Point(18, 122),
                         state.selected_setting == MOVING_CONFIRM_TIME ? selected_color : normal_color);
    draw_outlined_status(ctx, line5, cv::Point(18, 152),
                         state.selected_setting == STILL_CONFIRM_TIME ? selected_color : normal_color);
    draw_outlined_status(ctx, line6, cv::Point(18, 182),
                         result.hook_motion_latched ? cv::Scalar(0, 165, 255)
                                                    : cv::Scalar(0, 220, 0));
}

REGISTER_LOGIC(logic_crane_motion);
REGISTER_LOGIC_ACTION(logic_crane_motion, logic_crane_motion_action);
