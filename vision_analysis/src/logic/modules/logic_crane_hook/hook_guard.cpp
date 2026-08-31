#include "logic/modules/logic_crane_hook/hook_guard.h"

#include <algorithm>
#include <cmath>

#include "logic/modules/logic_crane_hook/detection_utils.h"

namespace crane_safety
{

const AlgoResult *HookGuard::select_target(const std::vector<AlgoResult> &results, const HookConfig &config) const
{
    const AlgoResult *best_track = nullptr;
    const AlgoResult *highest = nullptr;
    float highest_score = -1.0f;

    for (const AlgoResult &result : results)
    {
        if (result.class_id != config.class_id || result.score < config.min_score)
            continue;
        if (last_track_id_ >= 0 && result.track_id == last_track_id_)
            best_track = &result;
        if (result.score > highest_score)
        {
            highest_score = result.score;
            highest = &result;
        }
    }

    if (best_track)
        return best_track;
    return highest;
}

void HookGuard::clear_detection_state(bool report_clear, HookResult *out)
{
    if (out && report_clear && alarm_)
        out->cleared = true;
    have_center_ = false;
    last_track_id_ = -1;
    missing_since_ms_ = 0;
    outside_state_ = false;
    alarm_ = false;
    outside_since_ms_ = 0;
    safe_since_ms_ = 0;
}

HookResult HookGuard::update(const std::vector<AlgoResult> &results, const cv::Size &frame_size,
                             uint64_t now_ms, const HookConfig &config)
{
    HookResult out;
    if (!config.enabled || frame_size.width <= 0 || frame_size.height <= 0)
    {
        clear_detection_state(true, &out);
        return out;
    }

    const AlgoResult *target = select_target(results, config);
    if (!target)
    {
        if (missing_since_ms_ == 0)
            missing_since_ms_ = now_ms;
        const uint64_t missing_ms = now_ms - missing_since_ms_;
        out.missing_elapsed_ms = std::min(missing_ms, config.lost_tolerance_ms);
        if (have_center_ && missing_ms <= config.lost_tolerance_ms)
        {
            out.held_during_loss = true;
            out.center = last_center_;
            const cv::Point frame_center(frame_size.width / 2, frame_size.height / 2);
            const float dx = static_cast<float>(out.center.x - frame_center.x);
            const float dy = static_cast<float>(out.center.y - frame_center.y);
            out.distance = std::sqrt(dx * dx + dy * dy);
            out.outside = outside_state_;
            out.alarm = alarm_;
            out.track_id = last_track_id_;
            out.outside_elapsed_ms = outside_since_ms_ == 0 ? 0 : now_ms - outside_since_ms_;
            out.safe_elapsed_ms = safe_since_ms_ == 0 ? 0 : now_ms - safe_since_ms_;
            return out;
        }

        /* 已经进入告警后，目标丢失不能被当作“回到圆内”。保持告警和喇叭，直到再次
         * 看见吊钩并在圆内连续达到 clear_ms。尚未触发告警时，丢失不会产生新告警。 */
        have_center_ = false;
        last_track_id_ = -1;
        safe_since_ms_ = 0;
        if (!alarm_)
        {
            outside_state_ = false;
            outside_since_ms_ = 0;
        }
        out.outside = outside_state_;
        out.alarm = alarm_;
        out.outside_elapsed_ms = outside_since_ms_ == 0 ? 0 : now_ms - outside_since_ms_;
        return out;
    }

    missing_since_ms_ = 0;
    out.visible = true;
    out.score = target->score;
    out.track_id = target->track_id;
    last_center_ = target->box_center();
    have_center_ = true;
    last_track_id_ = target->track_id;
    out.center = last_center_;

    const cv::Point frame_center(frame_size.width / 2, frame_size.height / 2);
    const float dx = static_cast<float>(out.center.x - frame_center.x);
    const float dy = static_cast<float>(out.center.y - frame_center.y);
    out.distance = std::sqrt(dx * dx + dy * dy);

    outside_state_ = out.distance > static_cast<float>(config.safe_radius);
    out.outside = outside_state_;

    if (outside_state_)
    {
        safe_since_ms_ = 0;
        if (outside_since_ms_ == 0)
            outside_since_ms_ = now_ms;
        if (!alarm_)
        {
            if (config.confirm_ms == 0 || now_ms - outside_since_ms_ >= config.confirm_ms)
            {
                alarm_ = true;
                out.triggered = true;
            }
        }
    }
    else
    {
        outside_since_ms_ = 0;
        if (alarm_)
        {
            if (safe_since_ms_ == 0)
                safe_since_ms_ = now_ms;
            if (config.clear_ms == 0 || now_ms - safe_since_ms_ >= config.clear_ms)
            {
                alarm_ = false;
                safe_since_ms_ = 0;
                out.cleared = true;
            }
        }
    }
    out.alarm = alarm_;
    out.outside_elapsed_ms = outside_since_ms_ == 0 ? 0 : now_ms - outside_since_ms_;
    out.safe_elapsed_ms = safe_since_ms_ == 0 ? 0 : now_ms - safe_since_ms_;
    return out;
}

void HookGuard::reset()
{
    clear_detection_state(false, nullptr);
}

} // namespace crane_safety
