/*
 * 行车吊钩歪拉斜吊检测
 *
 * 业务逻辑：
 * 1. 以画面中心为圆心建立安全圆。
 * 2. 吊钩连续出圈达到阈值后上报一次告警。
 * 3. 同一次持续出圈不重复上报。
 * 4. 告警后，吊钩必须连续回圈达到冷却时间，才允许下一轮告警。
 * 5. 冷却期间再次出圈，已累计的冷却时间清零。
 * 6. 目标丢失不等同于恢复安全；短时丢帧暂停计时，长时丢失进入异常状态。
 *
 */

#include "logic/core/logic_common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace
{

constexpr float kThresholdMinSec = 0.1f;
constexpr float kThresholdMaxSec = 3600.0f;
constexpr float kCooldownMinSec = 0.5f;
constexpr float kCooldownMaxSec = 3600.0f;
constexpr float kTimeStepSec = 0.1f;

constexpr int kSafeRadiusMinPx = 20;
constexpr int kSafeRadiusMaxPx = 320;
constexpr int kRadiusStepPx = 5;

constexpr float kDefaultLostToleranceSec = 0.3f;
constexpr uint64_t kClockGapResetMs = 2000;

enum class HookState
{
    NORMAL,     // 安全，可触发下一轮告警
    EXCEEDING,  // 吊钩在圈外，连续计时中
    ALARMED,    // 已经触发本轮告警，等待吊钩回圈
    COOLING,    // 吊钩已经回圈，连续冷却中
    TARGET_LOST // 吊钩目标长时间丢失，不视为安全
};

struct HookDetectState
{
    std::mutex mutex;

    HookState state = HookState::NORMAL;
    bool initialized = false;
    bool alarm_latched = false;

    uint64_t exceed_start_ms = 0;
    uint64_t cooldown_start_ms = 0;
    uint64_t target_lost_start_ms = 0;
    uint64_t last_timestamp_ms = 0;

    std::string target_label;
    float threshold_sec = 0.0f;
    float cooldown_sec = 0.0f;
    float target_lost_tolerance_sec = kDefaultLostToleranceSec;
    float min_target_score = 0.0f;
    int safe_radius = 0;

    bool adjusting_cooldown = false;
    bool target_loss_active = false;
    bool last_target_valid = false;
    bool last_inside = true;
};

template <typename T> T clamp_value(T value, T min_value, T max_value)
{
    return std::max(min_value, std::min(value, max_value));
}

const char *state_text(HookState state)
{
    switch (state)
    {
    case HookState::NORMAL:
        return "安全";
    case HookState::EXCEEDING:
        return "圈外计时中";
    case HookState::ALARMED:
        return "危险已告警";
    case HookState::COOLING:
        return "圈内冷却中";
    case HookState::TARGET_LOST:
        return "吊钩目标丢失";
    }
    return "未知";
}

void reset_detection_cycle(HookDetectState &s)
{
    s.state = HookState::NORMAL;
    s.alarm_latched = false;
    s.exceed_start_ms = 0;
    s.cooldown_start_ms = 0;
    s.target_lost_start_ms = 0;
    s.target_loss_active = false;
    s.last_inside = true;
}

void enter_target_lost(HookDetectState &s)
{
    if (s.state == HookState::ALARMED || s.state == HookState::COOLING)
        s.alarm_latched = true;

    s.state = HookState::TARGET_LOST;
    s.exceed_start_ms = 0;
    s.cooldown_start_ms = 0;
}

void handle_clock_discontinuity(HookDetectState &s, uint64_t now)
{
    if (s.state == HookState::EXCEEDING)
    {
        // 时间不连续，不能继续认定为“连续出圈”。
        s.state = HookState::NORMAL;
        s.alarm_latched = false;
    }
    else if (s.state == HookState::COOLING)
    {
        // 时间不连续，不能继续认定为“连续回圈”。
        s.state = HookState::ALARMED;
        s.alarm_latched = true;
    }

    s.exceed_start_ms = 0;
    s.cooldown_start_ms = 0;

    if (s.state == HookState::TARGET_LOST)
    {
        s.target_loss_active = true;
        s.target_lost_start_ms = now;
    }
    else
    {
        s.target_loss_active = false;
        s.target_lost_start_ms = 0;
    }
}

bool initialize_runtime_parameters(ChannelContext *ctx, HookDetectState &s)
{
    if (s.initialized)
        return true;

    const std::string cfg_target_label = ctx->param_string("target_label");
    const float cfg_threshold_sec = ctx->param_float("exceed_threshold_sec");
    const float cfg_cooldown_sec = ctx->param_float("cooldown_sec");
    const int cfg_safe_radius = static_cast<int>(ctx->param_int("safe_radius"));

    float cfg_lost_tolerance_sec = ctx->param_float("target_lost_tolerance_sec");
    if (cfg_lost_tolerance_sec <= 0.0f)
        cfg_lost_tolerance_sec = kDefaultLostToleranceSec;

    const float cfg_min_target_score = std::max(0.0f, ctx->param_float("min_target_score"));

    if (cfg_target_label.empty())
    {
        draw_text(ctx, "hook: 请配置 target_label", cv::Point(20, 34), cv::Scalar(0, 165, 255), 0.7, 3);
        return false;
    }

    // 按钮可能在第一帧之前已经修改运行值；只有尚未设置的值才采用配置。
    if (s.threshold_sec <= 0.0f)
        s.threshold_sec = cfg_threshold_sec;
    if (s.cooldown_sec <= 0.0f)
        s.cooldown_sec = cfg_cooldown_sec;
    if (s.safe_radius <= 0)
        s.safe_radius = cfg_safe_radius;

    if (s.threshold_sec < kThresholdMinSec || s.threshold_sec > kThresholdMaxSec)
    {
        draw_text(ctx, "hook: exceed_threshold_sec 配置非法", cv::Point(20, 34), cv::Scalar(0, 165, 255), 0.7, 3);
        return false;
    }

    if (s.cooldown_sec < kCooldownMinSec || s.cooldown_sec > kCooldownMaxSec)
    {
        draw_text(ctx, "hook: cooldown_sec 配置非法", cv::Point(20, 34), cv::Scalar(0, 165, 255), 0.7, 3);
        return false;
    }

    if (s.safe_radius < kSafeRadiusMinPx || s.safe_radius > kSafeRadiusMaxPx)
    {
        draw_text(ctx, "hook: safe_radius 需要在 20~320 之间", cv::Point(20, 34), cv::Scalar(0, 165, 255), 0.7, 3);
        return false;
    }

    s.target_label = cfg_target_label;
    s.target_lost_tolerance_sec = cfg_lost_tolerance_sec;
    s.min_target_score = cfg_min_target_score;
    s.initialized = true;
    return true;
}

ChannelActionResult logic_hook_action(ChannelContext *ctx, const ChannelAction *action)
{
    ChannelActionResult result;
    if (!ctx || !ctx->state || !action)
    {
        result.message = "ctx, state or action is null";
        return result;
    }

    if (!*ctx->state)
        *ctx->state = std::make_shared<HookDetectState>();

    auto state = std::static_pointer_cast<HookDetectState>(*ctx->state);
    HookDetectState &s = *state;
    std::lock_guard<std::mutex> lock(s.mutex);

    if (action->name == "toggle_adjust_target")
    {
        s.adjusting_cooldown = !s.adjusting_cooldown;
        result.handled = true;
        result.message = s.adjusting_cooldown ? "当前调整目标: 冷却时间" : "当前调整目标: 超限阈值";
        return result;
    }

    if (action->name == "increase_value" || action->name == "decrease_value")
    {
        const float delta = action->name == "increase_value" ? kTimeStepSec : -kTimeStepSec;
        char message[96];

        if (s.adjusting_cooldown)
        {
            const float base = s.cooldown_sec > 0.0f ? s.cooldown_sec : kCooldownMinSec;
            s.cooldown_sec = clamp_value(base + delta, kCooldownMinSec, kCooldownMaxSec);
            std::snprintf(message, sizeof(message), "冷却时间已调整为 %.1f 秒", static_cast<double>(s.cooldown_sec));
        }
        else
        {
            const float base = s.threshold_sec > 0.0f ? s.threshold_sec : kThresholdMinSec;
            s.threshold_sec = clamp_value(base + delta, kThresholdMinSec, kThresholdMaxSec);
            std::snprintf(message, sizeof(message), "超限阈值已调整为 %.1f 秒", static_cast<double>(s.threshold_sec));
        }

        result.handled = true;
        result.message = message;
        return result;
    }

    if (action->name == "increase_radius" || action->name == "decrease_radius")
    {
        const int delta = action->name == "increase_radius" ? kRadiusStepPx : -kRadiusStepPx;
        const int base = s.safe_radius > 0 ? s.safe_radius : kSafeRadiusMinPx;
        s.safe_radius = clamp_value(base + delta, kSafeRadiusMinPx, kSafeRadiusMaxPx);

        char message[96];
        std::snprintf(message, sizeof(message), "安全半径已调整为 %d px", s.safe_radius);
        result.handled = true;
        result.message = message;
        return result;
    }

    if (action->name == "reset_alarm")
    {
        if (s.state != HookState::NORMAL && (!s.last_target_valid || !s.last_inside))
        {
            result.handled = true;
            result.message = "复位被拒绝: 尚未确认吊钩位于安全圈内";
            return result;
        }

        reset_detection_cycle(s);
        result.handled = true;
        result.message = "检测状态已复位";
        return result;
    }

    result.message = std::string("不支持的动作: ") + action->name;
    return result;
}

void logic_hook(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->frame || ctx->frame->empty())
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<HookDetectState>();

    auto state = std::static_pointer_cast<HookDetectState>(*ctx->state);
    HookDetectState &s = *state;
    std::lock_guard<std::mutex> lock(s.mutex);

    if (!initialize_runtime_parameters(ctx, s))
        return;

    const uint64_t now = ctx->timestamp_ms;
    if (s.last_timestamp_ms > 0)
    {
        const bool timestamp_rollback = now < s.last_timestamp_ms;
        const bool timestamp_gap = !timestamp_rollback && (now - s.last_timestamp_ms) > kClockGapResetMs;
        if (timestamp_rollback || timestamp_gap)
            handle_clock_discontinuity(s, now);
    }
    s.last_timestamp_ms = now;

    const uint64_t lost_tolerance_ms = static_cast<uint64_t>(s.target_lost_tolerance_sec * 1000.0f);

    const int cx = ctx->frame->cols / 2;
    const int cy = ctx->frame->rows / 2;
    const cv::Point screen_center(cx, cy);

    AlgoResult *hook_target = nullptr;
    float best_score = -1.0f;
    if (ctx->results)
    {
        for (auto &result : *ctx->results)
        {
            if (result.label == s.target_label && result.score >= s.min_target_score && result.score > best_score)
            {
                best_score = result.score;
                hook_target = &result;
            }
        }
    }

    cv::Point hook_center(-1, -1);
    float center_distance_px = 0.0f;
    bool hook_inside = false;

    bool just_alarmed = false;
    float alarm_actual_exceed_sec = 0.0f;

    if (!hook_target)
    {
        s.last_target_valid = false;

        if (!s.target_loss_active)
        {
            s.target_loss_active = true;
            s.target_lost_start_ms = now;
        }

        const uint64_t lost_elapsed_ms = now - s.target_lost_start_ms;
        if (lost_elapsed_ms >= lost_tolerance_ms && s.state != HookState::TARGET_LOST)
            enter_target_lost(s);
    }
    else
    {
        hook_center = hook_target->box_center();
        const int64_t dx = static_cast<int64_t>(hook_center.x) - cx;
        const int64_t dy = static_cast<int64_t>(hook_center.y) - cy;
        center_distance_px = std::sqrt(static_cast<float>(dx * dx + dy * dy));

        // 不使用迟滞区：安全圆边界以内为圈内，超过边界为圈外。
        hook_inside = center_distance_px <= static_cast<float>(s.safe_radius);

        const bool had_target_loss = s.target_loss_active;
        const uint64_t lost_elapsed_ms = had_target_loss ? now - s.target_lost_start_ms : 0;

        if (had_target_loss && lost_elapsed_ms >= lost_tolerance_ms && s.state != HookState::TARGET_LOST)
            enter_target_lost(s);

        if (s.state == HookState::TARGET_LOST)
        {
            // 长时间丢失后重新发现目标，连续计时必须重新开始。
            if (s.alarm_latched)
            {
                if (hook_inside)
                {
                    s.state = HookState::COOLING;
                    s.cooldown_start_ms = now;
                }
                else
                {
                    s.state = HookState::ALARMED;
                }
            }
            else
            {
                if (hook_inside)
                {
                    s.state = HookState::NORMAL;
                }
                else
                {
                    s.state = HookState::EXCEEDING;
                    s.exceed_start_ms = now;
                }
            }
        }
        else
        {
            // 短时丢帧只暂停计时，不把丢帧时间算入连续时间。
            if (had_target_loss && lost_elapsed_ms < lost_tolerance_ms)
            {
                if (s.state == HookState::EXCEEDING)
                    s.exceed_start_ms += lost_elapsed_ms;
                else if (s.state == HookState::COOLING)
                    s.cooldown_start_ms += lost_elapsed_ms;
            }

            if (hook_inside)
            {
                switch (s.state)
                {
                case HookState::NORMAL:
                    break;

                case HookState::EXCEEDING:
                    // 告警前回圈，本次超限取消。
                    s.state = HookState::NORMAL;
                    s.exceed_start_ms = 0;
                    break;

                case HookState::ALARMED:
                    // 告警后首次回圈，开始连续冷却。
                    s.state = HookState::COOLING;
                    s.cooldown_start_ms = now;
                    break;

                case HookState::COOLING:
                    if ((now - s.cooldown_start_ms) >= static_cast<uint64_t>(s.cooldown_sec * 1000.0f))
                    {
                        reset_detection_cycle(s);
                        s.last_target_valid = true;
                        s.last_inside = true;
                    }
                    break;

                case HookState::TARGET_LOST:
                    break;
                }
            }
            else
            {
                switch (s.state)
                {
                case HookState::NORMAL:
                    s.state = HookState::EXCEEDING;
                    s.exceed_start_ms = now;
                    break;

                case HookState::EXCEEDING: {
                    const uint64_t exceed_elapsed_ms = now - s.exceed_start_ms;
                    if (exceed_elapsed_ms >= static_cast<uint64_t>(s.threshold_sec * 1000.0f))
                    {
                        s.state = HookState::ALARMED;
                        s.alarm_latched = true;
                        just_alarmed = true;
                        alarm_actual_exceed_sec = exceed_elapsed_ms / 1000.0f;
                    }
                    break;
                }

                case HookState::ALARMED:
                    // 同一次持续出圈不重复上报。
                    break;

                case HookState::COOLING:
                    // 冷却中再次出圈：累计冷却时间清零，回到已告警状态。
                    s.state = HookState::ALARMED;
                    s.cooldown_start_ms = 0;
                    s.alarm_latched = true;
                    break;

                case HookState::TARGET_LOST:
                    break;
                }
            }
        }

        s.target_loss_active = false;
        s.target_lost_start_ms = 0;
        s.last_target_valid = true;
        s.last_inside = hook_inside;
    }

    const cv::Scalar GREEN(0, 238, 0);
    const cv::Scalar YELLOW(0, 255, 255);
    const cv::Scalar RED(0, 0, 230);
    const cv::Scalar ORANGE(0, 140, 255);
    const cv::Scalar WHITE(255, 255, 255);
    const cv::Scalar CYAN(255, 255, 0);
    const cv::Scalar COOLING_COLOR(255, 180, 60);
    const cv::Scalar LOST_COLOR(255, 0, 255);
    const cv::Scalar BLACK(30, 30, 30);

    cv::Scalar circle_color = GREEN;
    int circle_thickness = 2;
    switch (s.state)
    {
    case HookState::NORMAL:
        circle_color = GREEN;
        break;
    case HookState::EXCEEDING:
        circle_color = YELLOW;
        circle_thickness = 3;
        break;
    case HookState::ALARMED:
        circle_color = ORANGE;
        circle_thickness = 4;
        break;
    case HookState::COOLING:
        circle_color = COOLING_COLOR;
        circle_thickness = 3;
        break;
    case HookState::TARGET_LOST:
        circle_color = LOST_COLOR;
        circle_thickness = 3;
        break;
    }

    draw_circle(ctx, screen_center, s.safe_radius, circle_color, circle_thickness);

    /* 左上角帧时间戳 */
    {
        const std::string ts = ctx->time_str();
        draw_text(ctx, ts.c_str(), cv::Point(12, 30), BLACK, 0.55, 3);
        draw_text(ctx, ts.c_str(), cv::Point(12, 30), WHITE, 0.55, 1);
    }

    if (hook_target)
    {
        hook_target->box_color = circle_color;
        draw_circle(ctx, hook_center, 5, circle_color, -1);
    }

    const int text_x = std::max(20, ctx->frame->cols - 280);
    int text_y = 34;
    char line[192];

    // 短时丢失期间冻结显示计时，避免画面显示继续增长。
    const uint64_t display_now = (!hook_target && s.target_loss_active) ? s.target_lost_start_ms : now;

    float exceed_elapsed_sec = 0.0f;
    if (s.state == HookState::EXCEEDING && display_now >= s.exceed_start_ms)
        exceed_elapsed_sec = (display_now - s.exceed_start_ms) / 1000.0f;
    else if (s.state == HookState::ALARMED || s.state == HookState::COOLING)
        exceed_elapsed_sec = s.threshold_sec;

    std::snprintf(line, sizeof(line), "超限持续时间 %.1f / %.1f 秒", static_cast<double>(exceed_elapsed_sec),
                  static_cast<double>(s.threshold_sec));
    draw_text(ctx, line, cv::Point(text_x, text_y), BLACK, 0.72, 4);
    draw_text(ctx, line, cv::Point(text_x, text_y),
              s.state == HookState::ALARMED ? ORANGE : (s.state == HookState::EXCEEDING ? YELLOW : WHITE), 0.72, 1);
    if (!s.adjusting_cooldown)
        draw_text(ctx, ">", cv::Point(text_x - 22, text_y), CYAN, 0.7, 1);

    text_y += 36;
    float cooldown_elapsed_sec = 0.0f;
    if (s.state == HookState::COOLING && display_now >= s.cooldown_start_ms)
        cooldown_elapsed_sec = (display_now - s.cooldown_start_ms) / 1000.0f;

    std::snprintf(line, sizeof(line), "告警冷却 %.1f / %.1f 秒", static_cast<double>(cooldown_elapsed_sec),
                  static_cast<double>(s.cooldown_sec));
    draw_text(ctx, line, cv::Point(text_x, text_y), BLACK, 0.72, 4);
    draw_text(ctx, line, cv::Point(text_x, text_y), s.state == HookState::COOLING ? COOLING_COLOR : WHITE, 0.72, 1);
    if (s.adjusting_cooldown)
        draw_text(ctx, ">", cv::Point(text_x - 22, text_y), CYAN, 0.7, 1);

    text_y += 36;
    const char *current_state_text = state_text(s.state);
    if (!hook_target && s.state != HookState::TARGET_LOST)
        current_state_text = "目标短时丢失，计时暂停";

    std::snprintf(line, sizeof(line), "状态: %s", current_state_text);
    draw_text(ctx, line, cv::Point(text_x, text_y), BLACK, 0.8, 5);
    draw_text(ctx, line, cv::Point(text_x, text_y), circle_color, 0.8, 2);

    /*
     * 只在 EXCEEDING -> ALARMED 的这一帧上报一次。
     * 不进入队列，不重试，不生成 alarm_cycle_id。
     * 必须放在全部绘制之后，保证告警图片包含本帧完整标注。
     */
    if (just_alarmed)
    {
        EventRequest request;
        request.event_type = "hook_oblique_pull";
        request.merge_mode = EventMergeMode::NEVER;

        const float exceed_distance_px = std::max(0.0f, center_distance_px - static_cast<float>(s.safe_radius));

        char message[256];
        std::snprintf(message, sizeof(message),
                      "吊钩歪拉斜吊: 中心距离 %.1fpx, 越界 %.1fpx, 连续超限 %.1fs, 吊钩坐标(%d,%d)",
                      static_cast<double>(center_distance_px), static_cast<double>(exceed_distance_px),
                      static_cast<double>(alarm_actual_exceed_sec), hook_center.x, hook_center.y);
        request.message = message;

        request.fields.set_string("target_label", s.target_label);
        request.fields.set_number("hook_center_x", hook_center.x);
        request.fields.set_number("hook_center_y", hook_center.y);
        request.fields.set_number("safe_radius", s.safe_radius);
        request.fields.set_number("center_distance", center_distance_px);
        request.fields.set_number("exceed_distance", exceed_distance_px);
        request.fields.set_number("actual_exceed_duration_seconds", alarm_actual_exceed_sec);
        request.fields.set_number("threshold_seconds", s.threshold_sec);

        const EventReportResult report = report_event(ctx, request);
        if (!report.accepted())
        {
            std::fprintf(stderr, "[logic_hook][ch%02d] report rejected: status=%s detail=%s\n", ctx->chnId,
                         event_report_status_name(report.status), report.detail.c_str());
        }
    }
}

} // namespace

REGISTER_LOGIC(logic_hook);
REGISTER_LOGIC_ACTION(logic_hook, logic_hook_action);
