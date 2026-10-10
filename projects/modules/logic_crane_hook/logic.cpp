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

namespace
{

enum TimeSetting
{
    OUTSIDE_CONFIRM_TIME = 0,
    INSIDE_RESET_TIME = 1,
    LOST_HOLD_TIME = 2
};

struct CenterCalibrationPoint
{
    double box_scale = 0.0;
    double center_x = 0.0;
    double center_y = 0.0;
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
    std::vector<CenterCalibrationPoint> center_points;
    std::string calibration_error;
    bool have_dynamic_center = false;
    cv::Point dynamic_center{-1, -1};
    double current_box_scale = 0.0;
    int dynamic_track_id = -1;
};

std::vector<std::string> split_csv_row(const std::string &line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ','))
        fields.push_back(field);
    return fields;
}

int column_index(const std::vector<std::string> &header, const char *name)
{
    const auto it = std::find(header.begin(), header.end(), name);
    return it == header.end() ? -1 : static_cast<int>(std::distance(header.begin(), it));
}

bool parse_number(const std::string &text, double *value)
{
    if (!value)
        return false;
    try
    {
        size_t used = 0;
        const double parsed = std::stod(text, &used);
        if (used != text.size() || !std::isfinite(parsed))
            return false;
        *value = parsed;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

std::vector<CenterCalibrationPoint> load_center_points(const std::string &path,
                                                       double scale_bin,
                                                       std::string *error)
{
    std::ifstream input(path);
    if (!input.is_open())
    {
        if (error)
            *error = "无法打开标定文件: " + path;
        return {};
    }

    std::string line;
    if (!std::getline(input, line))
    {
        if (error)
            *error = "标定文件为空: " + path;
        return {};
    }
    const std::vector<std::string> header = split_csv_row(line);
    const int scale_col = column_index(header, "box_scale");
    const int x_col = column_index(header, "center_x");
    const int y_col = column_index(header, "center_y");
    if (scale_col < 0 || x_col < 0 || y_col < 0)
    {
        if (error)
            *error = "标定CSV缺少 box_scale/center_x/center_y 列";
        return {};
    }

    std::vector<CenterCalibrationPoint> raw_points;
    const int max_col = std::max(scale_col, std::max(x_col, y_col));
    while (std::getline(input, line))
    {
        if (line.empty())
            continue;
        const std::vector<std::string> fields = split_csv_row(line);
        if (static_cast<int>(fields.size()) <= max_col)
            continue;
        CenterCalibrationPoint point;
        if (!parse_number(fields[scale_col], &point.box_scale) ||
            !parse_number(fields[x_col], &point.center_x) ||
            !parse_number(fields[y_col], &point.center_y) || point.box_scale <= 0.0)
            continue;
        raw_points.push_back(point);
    }
    if (raw_points.empty())
    {
        if (error)
            *error = "标定CSV中没有有效样本";
        return {};
    }

    std::sort(raw_points.begin(), raw_points.end(),
              [](const CenterCalibrationPoint &a, const CenterCalibrationPoint &b) {
                  return a.box_scale < b.box_scale;
              });

    const double bin_width = std::max(0.1, scale_bin);
    struct Bin
    {
        long long key = 0;
        double scale_sum = 0.0;
        double x_sum = 0.0;
        double y_sum = 0.0;
        int count = 0;
    };
    std::vector<Bin> bins;
    for (const CenterCalibrationPoint &point : raw_points)
    {
        const long long key = static_cast<long long>(std::llround(point.box_scale / bin_width));
        if (bins.empty() || bins.back().key != key)
            bins.push_back(Bin{key, 0.0, 0.0, 0.0, 0});
        Bin &bin = bins.back();
        bin.scale_sum += point.box_scale;
        bin.x_sum += point.center_x;
        bin.y_sum += point.center_y;
        ++bin.count;
    }

    std::vector<CenterCalibrationPoint> points;
    points.reserve(bins.size());
    for (const Bin &bin : bins)
    {
        points.push_back({bin.scale_sum / bin.count, bin.x_sum / bin.count,
                          bin.y_sum / bin.count});
    }
    if (error)
        error->clear();
    return points;
}

cv::Point interpolate_center(const std::vector<CenterCalibrationPoint> &points, double box_scale)
{
    if (points.empty())
        return {-1, -1};
    if (points.size() == 1 || box_scale <= points.front().box_scale)
        return {static_cast<int>(std::lround(points.front().center_x)),
                static_cast<int>(std::lround(points.front().center_y))};
    if (box_scale >= points.back().box_scale)
        return {static_cast<int>(std::lround(points.back().center_x)),
                static_cast<int>(std::lround(points.back().center_y))};

    const auto upper = std::upper_bound(
        points.begin(), points.end(), box_scale,
        [](double scale, const CenterCalibrationPoint &point) { return scale < point.box_scale; });
    const CenterCalibrationPoint &right = *upper;
    const CenterCalibrationPoint &left = *(upper - 1);
    const double span = right.box_scale - left.box_scale;
    const double ratio = span > 0.0 ? (box_scale - left.box_scale) / span : 0.0;
    return {static_cast<int>(std::lround(left.center_x + (right.center_x - left.center_x) * ratio)),
            static_cast<int>(std::lround(left.center_y + (right.center_y - left.center_y) * ratio))};
}

const AlgoResult *select_hook_for_scale(const std::vector<AlgoResult> &results,
                                        int class_id, float min_score,
                                        int preferred_track_id)
{
    const AlgoResult *same_track = nullptr;
    const AlgoResult *highest_score = nullptr;
    for (const AlgoResult &result : results)
    {
        if (result.class_id != class_id || result.score < min_score ||
            result.box.width <= 0 || result.box.height <= 0)
            continue;
        if (preferred_track_id >= 0 && result.track_id == preferred_track_id)
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
        state.center_points = load_center_points(
            ctx->param_string("center_calibration_path"),
            ctx->param_float("calibration_scale_bin"), &state.calibration_error);
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

void apply_dynamic_center(ChannelContext *ctx, HookLogicState &state,
                          crane_safety::HookConfig *config)
{
    if (!ctx || !ctx->results || !config || !ctx->param_bool("dynamic_center_enabled") ||
        state.center_points.empty())
        return;

    const AlgoResult *hook = select_hook_for_scale(*ctx->results, config->class_id,
                                                   config->min_score, state.dynamic_track_id);
    if (hook)
    {
        state.current_box_scale =
            std::sqrt(static_cast<double>(hook->box.width) * hook->box.height);
        state.dynamic_center = interpolate_center(state.center_points, state.current_box_scale);
        state.have_dynamic_center = state.dynamic_center.x >= 0 && state.dynamic_center.y >= 0;
        state.dynamic_track_id = hook->track_id;
    }
    if (state.have_dynamic_center)
    {
        config->safe_center_x = state.dynamic_center.x;
        config->safe_center_y = state.dynamic_center.y;
    }
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
    apply_dynamic_center(ctx, state, &config);
    const crane_safety::HookResult result =
        state.guard.update(*ctx->results, frame->size(), ctx->timestamp_ms, config);
    if (result.visible)
        state.dynamic_track_id = result.track_id;

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
                      ctx->param_bool("dynamic_center_enabled") && !state.center_points.empty());
    ctx->publish_number("hook_box_scale", state.current_box_scale);
    ctx->publish_int("hook_safe_center_x",
                     crane_safety::HookGuard::safe_center(frame->size(), config).x);
    ctx->publish_int("hook_safe_center_y",
                     crane_safety::HookGuard::safe_center(frame->size(), config).y);

    const cv::Point center = crane_safety::HookGuard::safe_center(frame->size(), config);
    draw_circle(ctx, center, config.safe_radius,
                result.alarm ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 200, 0), result.alarm ? 4 : 2);
    if (result.center.x >= 0 && result.center.y >= 0)
    {
        draw_circle(ctx, result.center, 5, cv::Scalar(0, 255, 255), -1);
    }

    const char *state_text = "安全待机";
    if (result.alarm && !result.visible)
        state_text = "目标丢失-告警保持";
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
    std::snprintf(line1, sizeof(line1), "吊钩: %s  状态: %s",
                  result.visible ? "可见" : (result.held_during_loss ? "短暂丢失" : "不可见"),
                  state_text);
    std::snprintf(line2, sizeof(line2), "偏移: %.1f/%dpx", result.distance, config.safe_radius);
    std::snprintf(line3, sizeof(line3), "置信度: %.2f/%.2f", result.score, config.min_score);
    std::snprintf(line4, sizeof(line4), "圆外确认: %.1f/%.1fs",
                  result.outside_elapsed_ms / 1000.0, config.confirm_ms / 1000.0);
    std::snprintf(line5, sizeof(line5), "安全复位: %.1f/%.1fs",
                  result.safe_elapsed_ms / 1000.0, config.clear_ms / 1000.0);
    std::snprintf(line6, sizeof(line6), "丢失保持: %.1f/%.1fs",
                  result.missing_elapsed_ms / 1000.0, config.lost_tolerance_ms / 1000.0);
    const bool dynamic_active = ctx->param_bool("dynamic_center_enabled") &&
                                !state.center_points.empty();
    std::snprintf(line7, sizeof(line7), "安全圆: (%d,%d)  R=%dpx  圆心:%s",
                  center.x, center.y, config.safe_radius,
                  dynamic_active ? "动态" : "固定/回退");
    if (dynamic_active)
    {
        std::snprintf(line8, sizeof(line8), "box_scale: %.1f  标定点:%zu  范围:%.1f~%.1f",
                      state.current_box_scale, state.center_points.size(),
                      state.center_points.front().box_scale, state.center_points.back().box_scale);
    }
    else if (ctx->param_bool("dynamic_center_enabled"))
    {
        std::snprintf(line8, sizeof(line8), "动态圆心未生效: %s",
                      state.calibration_error.empty() ? "没有有效标定样本" : state.calibration_error.c_str());
    }
    else
    {
        std::snprintf(line8, sizeof(line8), "动态圆心: 已关闭");
    }
    const cv::Scalar status_color = result.alarm ? cv::Scalar(0, 0, 255)
                                                : (result.outside ? cv::Scalar(0, 165, 255)
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
}

REGISTER_LOGIC(logic_crane_hook);
REGISTER_LOGIC_ACTION(logic_crane_hook, logic_crane_hook_action);
