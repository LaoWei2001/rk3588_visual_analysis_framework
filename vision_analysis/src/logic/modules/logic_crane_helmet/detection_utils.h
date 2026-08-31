#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "inference/inference_types.h"
#include "logic/core/channel_logic.h"

namespace crane_safety
{

static inline std::vector<std::string> split_labels(const std::string &csv)
{
    std::vector<std::string> labels;
    std::istringstream input(csv);
    std::string item;
    while (std::getline(input, item, ','))
    {
        const auto first = std::find_if_not(item.begin(), item.end(),
                                            [](unsigned char ch) { return std::isspace(ch) != 0; });
        const auto last = std::find_if_not(item.rbegin(), item.rend(),
                                           [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
        if (first < last)
            labels.emplace_back(first, last);
    }
    return labels;
}

static inline bool result_matches(const AlgoResult &result, const std::vector<std::string> &labels,
                                  float min_score)
{
    if (result.score < min_score)
        return false;
    return !labels.empty() && std::find(labels.begin(), labels.end(), result.label) != labels.end();
}

static inline bool foot_point_in_polygon(const AlgoResult &result, const RoiZone *zone)
{
    if (!zone || zone->polygon.size() < 3)
        return false;
    const cv::Point foot(result.box.x + result.box.width / 2, result.box.y + result.box.height);
    return cv::pointPolygonTest(zone->polygon, foot, false) >= 0;
}

static inline uint64_t seconds_to_ms(float seconds)
{
    if (!(seconds > 0.0f))
        return 0;
    return static_cast<uint64_t>(std::llround(static_cast<double>(seconds) * 1000.0));
}

} // namespace crane_safety
