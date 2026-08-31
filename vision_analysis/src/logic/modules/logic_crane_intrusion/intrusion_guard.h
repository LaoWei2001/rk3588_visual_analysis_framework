#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "inference/inference_types.h"
#include "logic/core/channel_logic.h"
#include "logic/modules/logic_crane_intrusion/violation_latch.h"

namespace crane_safety
{

struct IntrusionConfig
{
    bool enabled = false;
    std::vector<std::string> person_labels;
    float min_score = 0.4f;
    uint64_t confirm_ms = 300;
    uint64_t clear_ms = 1000;
};

struct IntrusionResult
{
    bool roi_available = false;
    bool alarm = false;
    bool triggered = false;
    bool cleared = false;
    int person_count = 0;
    uint64_t confirm_elapsed_ms = 0;
    uint64_t clear_elapsed_ms = 0;
    std::vector<int> track_ids;
};

class IntrusionGuard
{
  public:
    IntrusionResult update(const std::vector<AlgoResult> &results, const RoiZone *zone,
                           uint64_t now_ms, const IntrusionConfig &config);
    void reset();

  private:
    ViolationLatch latch_;
};

} // namespace crane_safety
