#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "inference/inference_types.h"
#include "logic/core/channel_logic.h"
#include "logic/modules/logic_crane_helmet/violation_latch.h"

namespace crane_safety
{

struct HelmetConfig
{
    bool enabled = false;
    std::vector<std::string> person_labels;
    std::vector<std::string> helmet_labels;
    float person_min_score = 0.4f;
    float helmet_min_score = 0.3f;
    float head_region_ratio = 0.4f;
    float match_margin_ratio = 0.12f;
    uint64_t confirm_ms = 500;
    uint64_t clear_ms = 1000;
};

struct HelmetResult
{
    bool roi_available = false;
    bool alarm = false;
    bool triggered = false;
    bool cleared = false;
    int person_count = 0;
    int unhelmeted_count = 0;
    uint64_t confirm_elapsed_ms = 0;
    uint64_t clear_elapsed_ms = 0;
    std::vector<int> unhelmeted_track_ids;
};

class HelmetGuard
{
  public:
    HelmetResult update(std::vector<AlgoResult> &results, const RoiZone *zone,
                        uint64_t now_ms, const HelmetConfig &config);
    void reset();

  private:
    ViolationLatch latch_;
};

} // namespace crane_safety
