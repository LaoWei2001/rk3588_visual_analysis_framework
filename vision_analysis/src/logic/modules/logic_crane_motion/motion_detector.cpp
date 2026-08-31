#include "logic/modules/logic_crane_motion/motion_detector.h"

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
                                    const MotionConfig &config)
{
    MotionResult out;
    out.moving = moving_;
    if (frame.empty())
        return out;

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

    const bool above_start = out.change_ratio >= config.start_ratio;
    const bool below_stop = out.change_ratio <= config.stop_ratio;

    if (!moving_)
    {
        stable_since_ms_ = 0;
        if (above_start)
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
        if (below_stop)
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
    moving_ = false;
    candidate_since_ms_ = 0;
    stable_since_ms_ = 0;
    suppress_until_ms_ = 0;
}

} // namespace crane_safety
