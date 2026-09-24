#include "logic/modules/logic_crane_helmet/helmet_guard.h"

#include <algorithm>

#include "logic/modules/logic_crane_helmet/detection_utils.h"

namespace crane_safety
{
namespace
{

struct CellRange
{
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
};

CellRange cell_range(const cv::Rect &box, int cell, int cols, int rows)
{
    CellRange range;
    range.x0 = std::max(0, box.x / cell);
    range.y0 = std::max(0, box.y / cell);
    range.x1 = std::min(cols - 1, (box.x + box.width) / cell);
    range.y1 = std::min(rows - 1, (box.y + box.height) / cell);
    return range;
}

// 把人员框登记进均匀网格，安全帽只需查询自身覆盖的少数格子，
// 使匹配复杂度从 O(P×H) 降为近似 O(P+H)。
std::vector<std::vector<int>> build_person_grid(const std::vector<AlgoResult *> &persons,
                                                int cols, int rows, int cell)
{
    std::vector<std::vector<int>> grid(static_cast<size_t>(cols) * rows);
    for (size_t index = 0; index < persons.size(); ++index)
    {
        const CellRange cells = cell_range(persons[index]->box, cell, cols, rows);
        for (int gy = cells.y0; gy <= cells.y1; ++gy)
            for (int gx = cells.x0; gx <= cells.x1; ++gx)
                grid[static_cast<size_t>(gy) * cols + gx].push_back(static_cast<int>(index));
    }
    return grid;
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
    int max_right = 0;
    int max_bottom = 0;
    for (AlgoResult &result : results)
    {
        max_right = std::max(max_right, result.box.x + result.box.width);
        max_bottom = std::max(max_bottom, result.box.y + result.box.height);
        if (result_matches(result, config.person_labels, config.person_min_score) &&
            foot_point_in_polygon(result, zone))
            persons.push_back(&result);
        else if (result_matches(result, config.helmet_labels, config.helmet_min_score))
            helmets.push_back(&result);
    }

    out.person_count = static_cast<int>(persons.size());
    std::vector<unsigned char> helmeted(persons.size(), 0);
    if (!persons.empty() && !helmets.empty())
    {
        constexpr int kCell = 64;
        const int cols = std::max(1, max_right / kCell + 1);
        const int rows = std::max(1, max_bottom / kCell + 1);
        const std::vector<std::vector<int>> grid = build_person_grid(persons, cols, rows, kCell);
        for (const AlgoResult *helmet : helmets)
        {
            const CellRange cells = cell_range(helmet->box, kCell, cols, rows);
            for (int gy = cells.y0; gy <= cells.y1; ++gy)
                for (int gx = cells.x0; gx <= cells.x1; ++gx)
                    for (int index : grid[static_cast<size_t>(gy) * cols + gx])
                    {
                        if (helmeted[index] || (persons[index]->box & helmet->box).area() == 0)
                            continue;
                        helmeted[index] = 1;
                    }
        }
    }

    for (size_t index = 0; index < persons.size(); ++index)
    {
        AlgoResult *person = persons[index];
        if (helmeted[index])
            person->box_color = cv::Scalar(0, 200, 0);
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
