#pragma once

#include <cstdint>
#include <deque>
#include <opencv2/opencv.hpp>

#include <channel.h>

namespace crane_safety
{

struct MotionConfig
{
    int diff_threshold = 10;
    int blur_kernel = 5;
    bool normalize_brightness = true;
    float change_ratio = 0.015f;
    uint64_t moving_confirm_ms = 200;
    uint64_t still_confirm_ms = 2000;
    float hook_motion_threshold_px = 3.0f;
    uint64_t hook_motion_window_ms = 200;
    uint64_t hook_still_reset_ms = 10000;
};

struct MotionResult
{
    bool initialized = false;
    bool moving = false;
    bool transition = false;
    bool suppressed = false;
    bool background_motion_detected = false;
    bool hook_visible = false;
    bool hook_moving = false;
    bool hook_motion_latched = false;
    float change_ratio = 0.0f;
    float hook_displacement = 0.0f;
    uint64_t moving_candidate_elapsed_ms = 0;
    uint64_t still_candidate_elapsed_ms = 0;
    uint64_t hook_still_reset_elapsed_ms = 0;
    /* ROI 外为0，ROI内超过灰度帧差阈值的像素为255。用于逐像素可视化。 */
    cv::Mat change_mask;
};

class MotionDetector
{
  public:
    MotionResult update(const cv::Mat &frame, const RoiZone *motion_zone, uint64_t now_ms,
                        const MotionConfig &config, const cv::Point *hook_center = nullptr,
                        int hook_track_id = -1);
    void suppress_after_lighting_change(uint64_t now_ms, uint64_t duration_ms);
    void reset();

  private:
    cv::Mat prepare_gray(const cv::Mat &frame, const MotionConfig &config) const;
    cv::Mat build_mask(const cv::Size &prepared_size, const cv::Size &source_size,
                       const RoiZone *motion_zone) const;
    void update_hook_motion(const cv::Point *hook_center, int hook_track_id,
                            uint64_t now_ms, const MotionConfig &config, MotionResult *out);

    struct HookPositionSample
    {
        cv::Point center;
        uint64_t timestamp_ms = 0;
    };

    cv::Mat previous_gray_;
    std::deque<HookPositionSample> hook_samples_;
    int hook_track_id_ = -1;
    bool hook_motion_latched_ = false;
    uint64_t hook_still_since_ms_ = 0;
    bool moving_ = false;
    uint64_t candidate_since_ms_ = 0;
    uint64_t stable_since_ms_ = 0;
    uint64_t suppress_until_ms_ = 0;
};

} // namespace crane_safety
