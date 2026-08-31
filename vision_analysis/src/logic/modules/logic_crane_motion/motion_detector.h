#pragma once

#include <cstdint>
#include <opencv2/opencv.hpp>

#include "logic/core/channel_logic.h"

namespace crane_safety
{

struct MotionConfig
{
    int diff_threshold = 25;
    int blur_kernel = 5;
    bool normalize_brightness = true;
    float start_ratio = 0.18f;
    float stop_ratio = 0.06f;
    uint64_t moving_confirm_ms = 400;
    uint64_t still_confirm_ms = 2000;
};

struct MotionResult
{
    bool initialized = false;
    bool moving = false;
    bool transition = false;
    bool suppressed = false;
    float change_ratio = 0.0f;
    uint64_t moving_candidate_elapsed_ms = 0;
    uint64_t still_candidate_elapsed_ms = 0;
    /* ROI 外为0，ROI内超过灰度帧差阈值的像素为255。用于逐像素可视化。 */
    cv::Mat change_mask;
};

class MotionDetector
{
  public:
    MotionResult update(const cv::Mat &frame, const RoiZone *motion_zone, uint64_t now_ms,
                        const MotionConfig &config);
    void suppress_after_lighting_change(uint64_t now_ms, uint64_t duration_ms);
    void reset();

  private:
    cv::Mat prepare_gray(const cv::Mat &frame, const MotionConfig &config) const;
    cv::Mat build_mask(const cv::Size &prepared_size, const cv::Size &source_size,
                       const RoiZone *motion_zone) const;

    cv::Mat previous_gray_;
    bool moving_ = false;
    uint64_t candidate_since_ms_ = 0;
    uint64_t stable_since_ms_ = 0;
    uint64_t suppress_until_ms_ = 0;
};

} // namespace crane_safety
