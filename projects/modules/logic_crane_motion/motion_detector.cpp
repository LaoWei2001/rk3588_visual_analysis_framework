#include "motion_detector.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace crane_safety
{

cv::Mat MotionDetector::prepare_gray(const cv::Mat &frame, const MotionConfig &config) const
{
    cv::Mat gray;
    if (frame.channels() == 1)
        gray = frame;
    else
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    if (config.normalize_brightness)
    {
        cv::Mat normalized;
        cv::equalizeHist(gray, normalized);
        gray = std::move(normalized);
    }

    int kernel = std::max(1, config.blur_kernel);
    if ((kernel & 1) == 0)
        ++kernel;
    if (kernel > 1)
        cv::GaussianBlur(gray, gray, cv::Size(kernel, kernel), 0.0);
    return gray;
}

cv::Mat MotionDetector::build_mask(const cv::Size &prepared_size, const cv::Size &source_size,
                                   const RoiZone *motion_zone) const
{
    cv::Mat mask(prepared_size, CV_8UC1, cv::Scalar(255));
    if (!motion_zone || motion_zone->polygon.size() < 3 || source_size.width <= 0 || source_size.height <= 0)
        return mask;

    mask.setTo(cv::Scalar(0));
    std::vector<cv::Point> scaled;
    scaled.reserve(motion_zone->polygon.size());
    const float sx = static_cast<float>(prepared_size.width) / source_size.width;
    const float sy = static_cast<float>(prepared_size.height) / source_size.height;
    for (const cv::Point &point : motion_zone->polygon)
        scaled.emplace_back(static_cast<int>(std::lround(point.x * sx)),
                            static_cast<int>(std::lround(point.y * sy)));
    const std::vector<std::vector<cv::Point>> polygons{scaled};
    cv::fillPoly(mask, polygons, cv::Scalar(255));
    return mask;
}

MotionResult MotionDetector::update(const cv::Mat &frame, const RoiZone *motion_zone, uint64_t now_ms,
                                    const MotionConfig &config, const cv::Point *hook_center,
                                    int hook_track_id)
{
    MotionResult out;
    out.moving = moving_;
    if (frame.empty())
        return out;

    update_hook_motion(hook_center, hook_track_id, now_ms, config, &out);

    /* 吊钩移动采用锁存状态：一次可靠移动即可放行运动判断；只有检测框持续可见且
     * 连续静止达到独立复位时间才解除锁存。检测框丢失不作为静止证据。 */
    if (out.hook_moving)
    {
        hook_motion_latched_ = true;
        hook_still_since_ms_ = 0;
    }
    else if (hook_motion_latched_ && out.hook_visible)
    {
        if (hook_still_since_ms_ == 0)
            hook_still_since_ms_ = now_ms;
        if (config.hook_still_reset_ms == 0 ||
            now_ms - hook_still_since_ms_ >= config.hook_still_reset_ms)
        {
            hook_motion_latched_ = false;
            hook_still_since_ms_ = 0;
        }
    }
    else if (!out.hook_visible)
    {
        /* 丢框期间既不复位，也不累计静止时间，重新检出后必须重新连续确认。 */
        hook_still_since_ms_ = 0;
    }
    out.hook_motion_latched = hook_motion_latched_;
    if (hook_motion_latched_ && hook_still_since_ms_ != 0 && now_ms >= hook_still_since_ms_)
        out.hook_still_reset_elapsed_ms = now_ms - hook_still_since_ms_;

    cv::Mat current = prepare_gray(frame, config);
    if (previous_gray_.empty() || previous_gray_.size() != current.size())
    {
        previous_gray_ = std::move(current);
        out.initialized = true;
        return out;
    }

    cv::Mat difference;
    cv::absdiff(current, previous_gray_, difference);
    previous_gray_ = std::move(current);

    cv::threshold(difference, difference, std::max(1, config.diff_threshold), 255, cv::THRESH_BINARY);
    static const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(difference, difference, cv::MORPH_OPEN, kernel);

    int valid_pixels = difference.rows * difference.cols;
    if (motion_zone && motion_zone->polygon.size() >= 3)
    {
        const cv::Mat mask = build_mask(difference.size(), frame.size(), motion_zone);
        cv::bitwise_and(difference, mask, difference);
        valid_pixels = cv::countNonZero(mask);
    }
    const int changed_pixels = cv::countNonZero(difference);
    out.change_ratio = valid_pixels > 0 ? static_cast<float>(changed_pixels) / valid_pixels : 0.0f;
    out.change_mask = difference;
    out.initialized = true;

    if (now_ms < suppress_until_ms_)
    {
        candidate_since_ms_ = 0;
        stable_since_ms_ = 0;
        out.suppressed = true;
        out.moving = moving_;
        return out;
    }

    /* 运动和静止共用同一个变化比例阈值。用互补条件避免
     * 变化比例恰好等于阈值时同时成为运动和静止候选。 */
    out.background_motion_detected = out.change_ratio >= config.change_ratio;
    const bool motion_detected = out.background_motion_detected && hook_motion_latched_;

    if (!moving_)
    {
        stable_since_ms_ = 0;
        if (motion_detected)
        {
            if (candidate_since_ms_ == 0)
                candidate_since_ms_ = now_ms;
            if (config.moving_confirm_ms == 0 || now_ms - candidate_since_ms_ >= config.moving_confirm_ms)
            {
                moving_ = true;
                candidate_since_ms_ = 0;
                out.transition = true;
            }
        }
        else
        {
            candidate_since_ms_ = 0;
        }
    }
    else
    {
        candidate_since_ms_ = 0;
        if (!motion_detected)
        {
            if (stable_since_ms_ == 0)
                stable_since_ms_ = now_ms;
            if (config.still_confirm_ms == 0 || now_ms - stable_since_ms_ >= config.still_confirm_ms)
            {
                moving_ = false;
                stable_since_ms_ = 0;
                out.transition = true;
            }
        }
        else
        {
            stable_since_ms_ = 0;
        }
    }

    out.moving = moving_;
    if (!moving_ && candidate_since_ms_ != 0 && now_ms >= candidate_since_ms_)
        out.moving_candidate_elapsed_ms = now_ms - candidate_since_ms_;
    if (moving_ && stable_since_ms_ != 0 && now_ms >= stable_since_ms_)
        out.still_candidate_elapsed_ms = now_ms - stable_since_ms_;
    return out;
}

void MotionDetector::update_hook_motion(const cv::Point *hook_center, int hook_track_id,
                                        uint64_t now_ms, const MotionConfig &config,
                                        MotionResult *out)
{
    if (!out)
        return;
    if (!hook_center)
    {
        hook_samples_.clear();
        hook_track_id_ = -1;
        return;
    }

    out->hook_visible = true;

    /* 目标切换后重新建立位移基准，避免把两个检测框之间的距离误当成吊钩运动。 */
    if (hook_track_id_ >= 0 && hook_track_id >= 0 && hook_track_id_ != hook_track_id)
        hook_samples_.clear();
    hook_track_id_ = hook_track_id;

    if (!hook_samples_.empty() && hook_samples_.back().timestamp_ms == now_ms)
        hook_samples_.back().center = *hook_center;
    else
        hook_samples_.push_back({*hook_center, now_ms});

    const uint64_t window_ms = std::max<uint64_t>(1U, config.hook_motion_window_ms);
    while (hook_samples_.size() > 1 && now_ms >= hook_samples_.front().timestamp_ms &&
           now_ms - hook_samples_.front().timestamp_ms > window_ms)
    {
        hook_samples_.pop_front();
    }

    int min_x = hook_samples_.front().center.x;
    int max_x = min_x;
    int min_y = hook_samples_.front().center.y;
    int max_y = min_y;
    for (const HookPositionSample &sample : hook_samples_)
    {
        min_x = std::min(min_x, sample.center.x);
        max_x = std::max(max_x, sample.center.x);
        min_y = std::min(min_y, sample.center.y);
        max_y = std::max(max_y, sample.center.y);
    }
    const float dx = static_cast<float>(max_x - min_x);
    const float dy = static_cast<float>(max_y - min_y);
    out->hook_displacement = std::sqrt(dx * dx + dy * dy);
    out->hook_moving = out->hook_displacement > std::max(0.0f, config.hook_motion_threshold_px);
}

void MotionDetector::suppress_after_lighting_change(uint64_t now_ms, uint64_t duration_ms)
{
    previous_gray_.release();
    candidate_since_ms_ = 0;
    stable_since_ms_ = 0;
    suppress_until_ms_ = now_ms + duration_ms;
}

void MotionDetector::reset()
{
    previous_gray_.release();
    hook_samples_.clear();
    hook_track_id_ = -1;
    hook_motion_latched_ = false;
    hook_still_since_ms_ = 0;
    moving_ = false;
    candidate_since_ms_ = 0;
    stable_since_ms_ = 0;
    suppress_until_ms_ = 0;
}

} // namespace crane_safety
