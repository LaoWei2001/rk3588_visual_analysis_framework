#include "hook_guard.h"

#include <algorithm>
#include <cmath>

#include "detection_utils.h"

namespace crane_safety
{

cv::Point HookGuard::safe_center(const cv::Size &frame_size, const HookConfig &config)
{
    return cv::Point(config.safe_center_x >= 0 ? config.safe_center_x : frame_size.width / 2,
                     config.safe_center_y >= 0 ? config.safe_center_y : frame_size.height / 2);
}

const AlgoResult *HookGuard::select_target(const std::vector<AlgoResult> &results, const HookConfig &config) const
{
    const AlgoResult *best_track = nullptr;
    const AlgoResult *highest = nullptr;
    float highest_score = -1.0f;

    for (const AlgoResult &result : results)
    {
        if (result.class_id != config.class_id || result.score < config.min_score ||
            result.box.width <= 0 || result.box.height <= 0)
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
    missing_ = confirming_ = clearing_ = false;
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
    HookObservation observation;
    const AlgoResult *target = select_target(results, config);
    if (target)
    {
        observation.visible = observation.valid = true;
        observation.center = target->box_center();
        observation.score = target->score;
        observation.track_id = target->track_id;
    }
    return update(observation, frame_size, now_ms, config);
}

HookResult HookGuard::update(const HookObservation &observation, const cv::Size &frame_size,
                             uint64_t now_ms, const HookConfig &config)
{
    HookResult out;
    if (!config.enabled || frame_size.width <= 0 || frame_size.height <= 0)
    {
        clear_detection_state(true, &out);
        return out;
    }
    out.visible = observation.visible;
    out.score = observation.score;
    out.track_id = observation.track_id;
    if (observation.visible && last_track_id_ >= 0 && observation.track_id != last_track_id_)
    {
        // A different hook must establish its own continuous timing and position.
        have_center_ = false;
        confirming_ = clearing_ = missing_ = false;
        outside_since_ms_ = safe_since_ms_ = 0;
        missing_since_ms_ = 0;
    }
    if (observation.visible)
        last_track_id_ = observation.track_id;
    if (!observation.visible || !observation.valid)
    {
        // Held positions are for display only. An unknown interval cannot confirm
        // an alarm or count toward the continuous safe period that clears one.
        confirming_ = clearing_ = false;
        outside_since_ms_ = safe_since_ms_ = 0;
        if (!missing_)
        {
            missing_since_ms_ = now_ms;
            missing_ = true;
        }
        const uint64_t missing_ms = now_ms - missing_since_ms_;
        out.missing_elapsed_ms = std::min(missing_ms, config.lost_tolerance_ms);
        if (have_center_ && missing_ms <= config.lost_tolerance_ms)
        {
            out.held_during_loss = true;
            out.center = last_center_;
            out.track_id = last_track_id_;
            const cv::Point circle_center = safe_center(frame_size, config);
            out.distance = static_cast<float>(cv::norm(out.center - circle_center));
        }
        else
        {
            have_center_ = false;
            if (!observation.visible) last_track_id_ = -1;
        }
        if (!alarm_) outside_state_ = false;
        out.outside = outside_state_;
        out.alarm = alarm_;
        return out;
    }

    missing_ = false;
    missing_since_ms_ = 0;
    out.measurement_valid = true;
    out.position_estimated = observation.estimated;
    last_center_ = observation.center;
    have_center_ = true;
    out.center = last_center_;

    const cv::Point circle_center = safe_center(frame_size, config);
    const float dx = static_cast<float>(out.center.x - circle_center.x);
    const float dy = static_cast<float>(out.center.y - circle_center.y);
    out.distance = std::sqrt(dx * dx + dy * dy);

    outside_state_ = out.distance > static_cast<float>(config.safe_radius);
    out.outside = outside_state_;

    if (outside_state_)
    {
        clearing_ = false;
        safe_since_ms_ = 0;
        if (!confirming_)
        {
            outside_since_ms_ = now_ms;
            confirming_ = true;
        }
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
        confirming_ = false;
        outside_since_ms_ = 0;
        if (observation.estimated)
        {
            clearing_ = false;
            safe_since_ms_ = 0;
        }
        else if (alarm_)
        {
            if (!clearing_)
            {
                safe_since_ms_ = now_ms;
                clearing_ = true;
            }
            if (config.clear_ms == 0 || now_ms - safe_since_ms_ >= config.clear_ms)
            {
                alarm_ = false;
                clearing_ = false;
                safe_since_ms_ = 0;
                out.cleared = true;
            }
        }
    }
    out.alarm = alarm_;
    out.outside_elapsed_ms = confirming_ ? now_ms - outside_since_ms_ : 0;
    out.safe_elapsed_ms = clearing_ ? now_ms - safe_since_ms_ : 0;
    return out;
}

void HookGuard::reset()
{
    clear_detection_state(false, nullptr);
}

} // namespace crane_safety
