#pragma once

#include <cstdint>
#include <vector>

#include <opencv2/opencv.hpp>

#include "inference/inference_types.h"

namespace crane_safety
{

struct HookConfig
{
    bool enabled = true;
    int class_id = 0;
    float min_score = 0.3f;
    int safe_radius = 150;
    uint64_t confirm_ms = 500;
    uint64_t clear_ms = 500;
    uint64_t lost_tolerance_ms = 500;
};

struct HookResult
{
    bool visible = false;
    bool held_during_loss = false;
    bool outside = false;
    bool alarm = false;
    bool triggered = false;
    bool cleared = false;
    cv::Point center{-1, -1};
    float distance = 0.0f;
    float score = 0.0f;
    int track_id = -1;
    uint64_t outside_elapsed_ms = 0;
    uint64_t safe_elapsed_ms = 0;
    uint64_t missing_elapsed_ms = 0;
};

class HookGuard
{
  public:
    HookResult update(const std::vector<AlgoResult> &results, const cv::Size &frame_size,
                      uint64_t now_ms, const HookConfig &config);
    void reset();

  private:
    const AlgoResult *select_target(const std::vector<AlgoResult> &results, const HookConfig &config) const;
    void clear_detection_state(bool report_clear, HookResult *out);

    bool have_center_ = false;
    cv::Point last_center_{-1, -1};
    int last_track_id_ = -1;
    uint64_t missing_since_ms_ = 0;
    bool outside_state_ = false;
    bool alarm_ = false;
    uint64_t outside_since_ms_ = 0;
    uint64_t safe_since_ms_ = 0;
};

} // namespace crane_safety
