#include "logic/modules/logic_crane_helmet/helmet_guard.h"

#include <algorithm>

#include "logic/modules/logic_crane_helmet/detection_utils.h"

namespace crane_safety
{
namespace
{

cv::Rect head_region(const AlgoResult &person, float head_ratio, float margin_ratio)
{
    const int margin_x = static_cast<int>(person.box.width * margin_ratio + 0.5f);
    const int margin_y = static_cast<int>(person.box.height * margin_ratio + 0.5f);
    const int height = std::max(1, static_cast<int>(person.box.height * head_ratio + 0.5f));
    return cv::Rect(person.box.x - margin_x, person.box.y - margin_y,
                    person.box.width + margin_x * 2, height + margin_y * 2);
}

} // namespace

HelmetResult HelmetGuard::update(std::vector<AlgoResult> &results, const RoiZone *zone,
                                 uint64_t now_ms, const HelmetConfig &config)
{
    HelmetResult out;
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

    std::vector<AlgoResult *> persons;
    std::vector<AlgoResult *> helmets;
    for (AlgoResult &result : results)
    {
        if (result_matches(result, config.person_labels, config.person_min_score) &&
            foot_point_in_polygon(result, zone))
            persons.push_back(&result);
        else if (result_matches(result, config.helmet_labels, config.helmet_min_score))
            helmets.push_back(&result);
    }

    out.person_count = static_cast<int>(persons.size());
    std::vector<unsigned char> helmet_used(helmets.size(), 0);
    for (AlgoResult *person : persons)
    {
        const cv::Rect region = head_region(*person, config.head_region_ratio, config.match_margin_ratio);
        int best_index = -1;
        int best_distance = 0;
        const cv::Point expected(person->box.x + person->box.width / 2, person->box.y);
        for (size_t index = 0; index < helmets.size(); ++index)
        {
            if (helmet_used[index])
                continue;
            const cv::Point center = helmets[index]->box_center();
            if (!region.contains(center))
                continue;
            const int dx = center.x - expected.x;
            const int dy = center.y - expected.y;
            const int distance = dx * dx + dy * dy;
            if (best_index < 0 || distance < best_distance)
            {
                best_index = static_cast<int>(index);
                best_distance = distance;
            }
        }
        if (best_index >= 0)
        {
            helmet_used[static_cast<size_t>(best_index)] = 1;
            person->box_color = cv::Scalar(0, 200, 0);
        }
        else
        {
            person->box_color = cv::Scalar(0, 0, 255);
            ++out.unhelmeted_count;
            if (person->track_id >= 0)
                out.unhelmeted_track_ids.push_back(person->track_id);
        }
    }

    const LatchUpdate latch = latch_.update(out.unhelmeted_count > 0, now_ms, config.confirm_ms, config.clear_ms);
    out.alarm = latch.active;
    out.triggered = latch.triggered;
    out.cleared = latch.cleared;
    out.confirm_elapsed_ms = latch.confirm_elapsed_ms;
    out.clear_elapsed_ms = latch.clear_elapsed_ms;
    return out;
}

void HelmetGuard::reset()
{
    latch_.reset();
}

} // namespace crane_safety
