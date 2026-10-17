#include <channel.h>
#include <drawing.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "detection_utils.h"
#include "hook_guard.h"
#include "hook_geometry.h"

namespace
{

enum TimeSetting
{
    OUTSIDE_CONFIRM_TIME = 0,
    INSIDE_RESET_TIME = 1,
    LOST_HOLD_TIME = 2
};

struct HookLogicState
{
    crane_safety::HookGuard guard;
    bool controls_initialized = false;
    int safe_radius = 130;
    uint64_t confirm_ms = 300;
    uint64_t clear_ms = 3000;
    uint64_t lost_tolerance_ms = 500;
    TimeSetting selected_time = OUTSIDE_CONFIRM_TIME;
    crane_safety::HookCalibration calibration;
    bool have_dynamic_center = false;
    cv::Point dynamic_center{-1, -1};
};

void draw_outlined_status(ChannelContext *ctx, const char *text, const cv::Point &pos,
                          const cv::Scalar &foreground)
{
    draw_text(ctx, text, pos, foreground, 0.7, 1, DrawCommand::ALL,
              /*shadow_enabled=*/true, cv::Scalar(15, 45, 90), 2);
}

void initialize_controls(ChannelContext *ctx, HookLogicState &state)
{
    if (state.controls_initialized)
        return;
    state.safe_radius = static_cast<int>(ctx->param_int("safe_radius"));
    state.confirm_ms = crane_safety::seconds_to_ms(ctx->param_float("confirm_sec"));
    state.clear_ms = crane_safety::seconds_to_ms(ctx->param_float("clear_sec"));
    state.lost_tolerance_ms = crane_safety::seconds_to_ms(ctx->param_float("lost_tolerance_sec"));
    if (ctx->param_bool("dynamic_center_enabled"))
    {
        state.calibration = crane_safety::load_hook_calibration(
            ctx->param_string("center_calibration_path"),
            ctx->param_float("calibration_scale_bin"), ctx->business_size(),
            static_cast<int>(ctx->param_int("edge_margin_px")));
    }
    state.controls_initialized = true;
}

const char *time_setting_name(TimeSetting setting)
{
    switch (setting)
    {
    case OUTSIDE_CONFIRM_TIME:
        return "圆外确认时间";
    case INSIDE_RESET_TIME:
        return "圆内安全复位时间";
    case LOST_HOLD_TIME:
        return "目标丢失保持时间";
    }
    return "未知时间";
}

uint64_t &selected_time_value(HookLogicState &state)
{
    if (state.selected_time == INSIDE_RESET_TIME)
        return state.clear_ms;
    if (state.selected_time == LOST_HOLD_TIME)
        return state.lost_tolerance_ms;
    return state.confirm_ms;
}

uint64_t selected_time_max_ms(const HookLogicState &state)
{
    return state.selected_time == LOST_HOLD_TIME ? 10000U : 60000U;
}

crane_safety::HookConfig read_config(ChannelContext *ctx, HookLogicState &state)
{
    initialize_controls(ctx, state);
    crane_safety::HookConfig config;
    config.enabled = true;
    config.class_id = static_cast<int>(ctx->param_int("hook_class_id"));
    config.min_score = ctx->param_float("min_score");
    config.safe_center_x = static_cast<int>(ctx->param_int("safe_center_x"));
    config.safe_center_y = static_cast<int>(ctx->param_int("safe_center_y"));
    config.safe_radius = state.safe_radius;
    config.confirm_ms = state.confirm_ms;
    config.clear_ms = state.clear_ms;
    config.lost_tolerance_ms = state.lost_tolerance_ms;
    return config;
}

} // namespace

static LogicActionResult logic_crane_hook_action(ChannelContext *ctx, const LogicAction *action)
{
    if (!ctx || !ctx->state || !action)
        return {false, "ctx or action is null"};
    if (!*ctx->state)
        *ctx->state = std::make_shared<HookLogicState>();
    HookLogicState &state = *std::static_pointer_cast<HookLogicState>(*ctx->state);
    initialize_controls(ctx, state);

    if (action->name == "radius_decrease")
    {
        state.safe_radius = std::max(10, state.safe_radius - 5);
        return {true, "安全圆半径已减小为 " + std::to_string(state.safe_radius) + " 像素"};
    }
    if (action->name == "radius_increase")
    {
        state.safe_radius = std::min(1000, state.safe_radius + 5);
        return {true, "安全圆半径已增大为 " + std::to_string(state.safe_radius) + " 像素"};
    }
    if (action->name == "switch_time_setting")
    {
        state.selected_time = static_cast<TimeSetting>((static_cast<int>(state.selected_time) + 1) % 3);
        return {true, std::string("当前设置项：") + time_setting_name(state.selected_time)};
    }
    if (action->name == "time_decrease" || action->name == "time_increase")
    {
        uint64_t &value = selected_time_value(state);
        if (action->name == "time_decrease")
            value = value >= 300U ? value - 300U : 0U;
        else
            value = std::min(selected_time_max_ms(state), value + 300U);
        char message[160];
        std::snprintf(message, sizeof(message), "%s已调整为 %.1f 秒",
                      time_setting_name(state.selected_time), value / 1000.0);
        return {true, message};
    }
    return {false, "unsupported action: " + action->name};
}

static void logic_crane_hook(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results)
        return;
    const cv::Mat *frame = ctx->model_frame();
    if (!frame || frame->empty())
        return;
    if (!*ctx->state)
        *ctx->state = std::make_shared<HookLogicState>();
    HookLogicState &state = *std::static_pointer_cast<HookLogicState>(*ctx->state);

    crane_safety::HookConfig config = read_config(ctx, state);
    const bool dynamic_enabled = ctx->param_bool("dynamic_center_enabled");
    const AlgoResult *hook = state.guard.select_target(*ctx->results, config);
    crane_safety::HookGeometry geometry;
    geometry.status = "未检测到吊钩";
    crane_safety::HookObservation observation;
    if (hook)
    {
        geometry = crane_safety::measure_hook(
            hook->box, ctx->business_size(), static_cast<int>(ctx->param_int("edge_margin_px")),
            state.calibration, dynamic_enabled);
        if (dynamic_enabled && state.calibration.points.empty())
            geometry.status = "动态标定不可用，请检查标定文件";
        observation.visible = true;
        observation.valid = geometry.valid;
        observation.estimated = geometry.estimated;
        observation.center = geometry.center;
        observation.score = hook->score;
        observation.track_id = hook->track_id;
        if (dynamic_enabled && geometry.valid && geometry.calibrated)
        {
            state.dynamic_center = geometry.safe_center;
            state.have_dynamic_center = true;
        }
    }
    if (dynamic_enabled && state.have_dynamic_center)
    {
        config.safe_center_x = state.dynamic_center.x;
        config.safe_center_y = state.dynamic_center.y;
    }
    const crane_safety::HookResult result =
        state.guard.update(observation, ctx->business_size(), ctx->timestamp_ms, config);
    const bool dynamic_active = dynamic_enabled && geometry.valid && geometry.calibrated;
    ctx->publish_bool("hook_measurement_valid", result.measurement_valid);
    ctx->publish_bool("hook_position_estimated", result.position_estimated);
    ctx->publish_bool("hook_truncated", geometry.truncated);
    ctx->publish_string("hook_geometry_status", geometry.status);
    ctx->publish_int("hook_box_width", hook ? hook->box.width : 0);
    ctx->publish_int("hook_box_height", hook ? hook->box.height : 0);

    ctx->publish_bool("hook_visible", result.visible);
    ctx->publish_bool("hook_held_during_loss", result.held_during_loss);
    ctx->publish_bool("hook_alarm", result.alarm);
    ctx->publish_number("hook_distance", result.distance);
    ctx->publish_int("hook_center_x", result.center.x);
    ctx->publish_int("hook_center_y", result.center.y);
    ctx->publish_number("hook_outside_elapsed_sec", result.outside_elapsed_ms / 1000.0);
    ctx->publish_number("hook_safe_elapsed_sec", result.safe_elapsed_ms / 1000.0);
    ctx->publish_number("hook_missing_elapsed_sec", result.missing_elapsed_ms / 1000.0);
    ctx->publish_bool("hook_dynamic_center_active",
                      dynamic_active);
    ctx->publish_number("hook_box_scale", hook ?
        std::sqrt(static_cast<double>(hook->box.width) * hook->box.height) : 0.0);
    ctx->publish_int("hook_safe_center_x",
                     crane_safety::HookGuard::safe_center(frame->size(), config).x);
    ctx->publish_int("hook_safe_center_y",
                     crane_safety::HookGuard::safe_center(frame->size(), config).y);

    const cv::Point center = crane_safety::HookGuard::safe_center(frame->size(), config);
    draw_circle(ctx, center, config.safe_radius,
                result.alarm ? cv::Scalar(0, 0, 255) :
                    (result.measurement_valid ? cv::Scalar(0, 200, 0) : cv::Scalar(0, 165, 255)),
                result.alarm ? 4 : 2);
    if (result.center.x >= 0 && result.center.y >= 0 &&
        result.center.x < frame->cols && result.center.y < frame->rows)
    {
        draw_circle(ctx, result.center, 5, cv::Scalar(0, 255, 255), -1);
    }

    const char *state_text = "安全待机";
    if (!result.measurement_valid)
        state_text = result.alarm ? "检测受限-告警保持" : "检测受限";
    else if (result.alarm && result.position_estimated && !result.outside)
        state_text = "位置估算-告警保持";
    else if (result.alarm && !result.outside)
        state_text = "圆内安全复位中";
    else if (result.alarm)
        state_text = "越界告警保持";
    else if (result.outside)
        state_text = "越界确认中";

    char line1[256];
    char line2[192];
    char line3[192];
    char line4[192];
    char line5[192];
    char line6[192];
    char line7[192];
    char line8[256];
    char line9[256];
    std::snprintf(line1, sizeof(line1), "吊钩: %s  状态: %s",
                  result.visible ? "可见" : (result.held_during_loss ? "短暂丢失" : "不可见"),
                  state_text);
    if (result.measurement_valid)
        std::snprintf(line2, sizeof(line2), "偏移: %.1f/%dpx%s", result.distance, config.safe_radius,
                      result.position_estimated ? " (估算，不能解除告警)" : "");
    else
        std::snprintf(line2, sizeof(line2), "偏移: 暂不可判定");
    std::snprintf(line3, sizeof(line3), "置信度: %.2f/%.2f", result.score, config.min_score);
    std::snprintf(line4, sizeof(line4), "圆外确认: %.1f/%.1fs",
                  result.outside_elapsed_ms / 1000.0, config.confirm_ms / 1000.0);
    std::snprintf(line5, sizeof(line5), "安全复位: %.1f/%.1fs",
                  result.safe_elapsed_ms / 1000.0, config.clear_ms / 1000.0);
    std::snprintf(line6, sizeof(line6), "丢失保持: %.1f/%.1fs",
                  result.missing_elapsed_ms / 1000.0, config.lost_tolerance_ms / 1000.0);
    std::snprintf(line7, sizeof(line7), "安全圆: (%d,%d)  R=%dpx  圆心:%s",
                  center.x, center.y, config.safe_radius,
                  dynamic_active ? "动态" : (dynamic_enabled ? "保持/待标定" : "固定"));
    std::snprintf(line8, sizeof(line8), "框高:%dpx  有效样本:%d  跳过:%d  范围:%.1f~%.1f",
                  hook ? hook->box.height : 0, state.calibration.accepted,
                  state.calibration.rejected, state.calibration.min_height, state.calibration.max_height);
    std::snprintf(line9, sizeof(line9), "%s", state.calibration.error.empty()
                  ? geometry.status.c_str() : state.calibration.error.c_str());
    const cv::Scalar status_color = result.alarm ? cv::Scalar(0, 0, 255)
                                                : ((!result.measurement_valid || result.outside) ? cv::Scalar(0, 165, 255)
                                                                  : cv::Scalar(240, 240, 240));
    draw_outlined_status(ctx, line1, cv::Point(18, 32), status_color);
    const cv::Scalar normal_time_color(240, 240, 240);
    const cv::Scalar selected_time_color(0, 255, 255);
    draw_outlined_status(ctx, line2, cv::Point(18, 62), normal_time_color);
    draw_outlined_status(ctx, line3, cv::Point(18, 92), normal_time_color);
    draw_outlined_status(ctx, line4, cv::Point(18, 122),
                         state.selected_time == OUTSIDE_CONFIRM_TIME ? selected_time_color : normal_time_color);
    draw_outlined_status(ctx, line5, cv::Point(18, 152),
                         state.selected_time == INSIDE_RESET_TIME ? selected_time_color : normal_time_color);
    draw_outlined_status(ctx, line6, cv::Point(18, 182),
                         state.selected_time == LOST_HOLD_TIME ? selected_time_color : normal_time_color);
    draw_outlined_status(ctx, line7, cv::Point(18, 212), normal_time_color);
    draw_outlined_status(ctx, line8, cv::Point(18, 242),
                         dynamic_active ? cv::Scalar(0, 255, 255) : normal_time_color);
    draw_outlined_status(ctx, line9, cv::Point(18, 272),
                         result.measurement_valid ? normal_time_color : cv::Scalar(0, 165, 255));
}

REGISTER_LOGIC(logic_crane_hook);
REGISTER_LOGIC_ACTION(logic_crane_hook, logic_crane_hook_action);
