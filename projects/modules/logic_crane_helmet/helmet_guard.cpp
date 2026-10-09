#include "helmet_guard.h"

#include <algorithm>

#include "detection_utils.h"

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
    if (mode_ != config.alarm_mode) { latch_.reset(); mode_ = config.alarm_mode; }
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
    std::vector<AlgoResult *> bare_heads;
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
        else if (config.alarm_mode == HelmetConfig::BareHead &&
                 result_matches(result, config.bare_head_labels, config.bare_head_min_score) &&
                 result.box.width > 0 && result.box.height > 0)
            bare_heads.push_back(&result);
    }

    out.person_count = static_cast<int>(persons.size());
    std::vector<unsigned char> helmeted(persons.size(), 0);
    std::vector<unsigned char> unhelmeted(persons.size(), 0);
    if (!persons.empty() && (!helmets.empty() || !bare_heads.empty()))
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
        // 裸头是正面违规证据，必须属于当前帧、ROI 内的某个人。
        // 同一裸头只分配给一个人员；重叠人框时优先覆盖头部最多、面积更小的框。
        for (const AlgoResult *head : bare_heads)
        {
            int best = -1;
            int best_overlap = 0;
            int best_person_area = 0;
            const CellRange cells = cell_range(head->box, kCell, cols, rows);
            for (int gy = cells.y0; gy <= cells.y1; ++gy)
                for (int gx = cells.x0; gx <= cells.x1; ++gx)
                    for (int index : grid[static_cast<size_t>(gy) * cols + gx])
                    {
                        const AlgoResult *person = persons[index];
                        if (person->frame_id != head->frame_id || !person->box.contains(head->box_center()))
                            continue;
                        const int overlap = (person->box & head->box).area();
                        const int area = person->box.area();
                        if (best < 0 || overlap > best_overlap || (overlap == best_overlap && area < best_person_area))
                        {
                            best = index; best_overlap = overlap; best_person_area = area;
                        }
                    }
            if (best >= 0) unhelmeted[best] = 1;
        }
    }

    for (size_t index = 0; index < persons.size(); ++index)
    {
        AlgoResult *person = persons[index];
        const bool violation = config.alarm_mode == HelmetConfig::BareHead ? unhelmeted[index] != 0 : helmeted[index] == 0;
        if (violation)
        {
            person->box_color = cv::Scalar(0, 0, 255);
            ++out.unhelmeted_count;
            if (person->track_id >= 0)
                out.unhelmeted_track_ids.push_back(person->track_id);
        }
        else if (helmeted[index]) person->box_color = cv::Scalar(0, 200, 0);
        else
        {
            // 未检测到帽子或裸头属于未知，不画成“未戴”或“已戴”。
            ++out.unknown_count;
            person->box_color = cv::Scalar(180, 180, 180);
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
