#include "logic/modules/logic_crane_intrusion/intrusion_guard.h"

#include "logic/modules/logic_crane_intrusion/detection_utils.h"

namespace crane_safety
{

IntrusionResult IntrusionGuard::update(const std::vector<AlgoResult> &results, const RoiZone *zone,
                                       uint64_t now_ms, const IntrusionConfig &config)
{
    IntrusionResult out;
    out.roi_available = zone && zone->polygon.size() >= 3;
    if (!config.enabled || !out.roi_available)
    {
        const LatchUpdate latch = latch_.update(false, now_ms, 0, config.clear_ms);
        out.alarm = latch.active;
        out.cleared = latch.cleared;
        out.confirm_elapsed_ms = latch.confirm_elapsed_ms;
        out.clear_elapsed_ms = latch.clear_elapsed_ms;
        return out;
    }

    for (const AlgoResult &result : results)
    {
        if (!result_matches(result, config.person_labels, config.min_score))
            continue;
        if (!foot_point_in_polygon(result, zone))
            continue;
        ++out.person_count;
        if (result.track_id >= 0)
            out.track_ids.push_back(result.track_id);
    }

    const LatchUpdate latch = latch_.update(out.person_count > 0, now_ms, config.confirm_ms, config.clear_ms);
    out.alarm = latch.active;
    out.triggered = latch.triggered;
    out.cleared = latch.cleared;
    out.confirm_elapsed_ms = latch.confirm_elapsed_ms;
    out.clear_elapsed_ms = latch.clear_elapsed_ms;
    return out;
}

void IntrusionGuard::reset()
{
    latch_.reset();
}

} // namespace crane_safety
