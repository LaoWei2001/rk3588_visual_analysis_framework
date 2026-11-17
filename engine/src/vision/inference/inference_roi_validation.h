#pragma once
#include <algorithm>
#include <cmath>
#include <config_types.h>

namespace vision
{
inline double orientation(const std::pair<double, double> &a, const std::pair<double, double> &b,
                          const std::pair<double, double> &c)
{
    return (b.first - a.first) * (c.second - a.second) - (b.second - a.second) * (c.first - a.first);
}

inline bool point_on_segment(const std::pair<double, double> &point, const std::pair<double, double> &a,
                             const std::pair<double, double> &b)
{
    return std::abs(orientation(a, b, point)) <= 1e-10 && point.first >= std::min(a.first, b.first) - 1e-10 &&
           point.first <= std::max(a.first, b.first) + 1e-10 && point.second >= std::min(a.second, b.second) - 1e-10 &&
           point.second <= std::max(a.second, b.second) + 1e-10;
}

inline bool segments_intersect(const std::pair<double, double> &a, const std::pair<double, double> &b,
                               const std::pair<double, double> &c, const std::pair<double, double> &d)
{
    const double o1 = orientation(a, b, c), o2 = orientation(a, b, d);
    const double o3 = orientation(c, d, a), o4 = orientation(c, d, b);
    if (((o1 > 1e-10 && o2 < -1e-10) || (o1 < -1e-10 && o2 > 1e-10)) &&
        ((o3 > 1e-10 && o4 < -1e-10) || (o3 < -1e-10 && o4 > 1e-10)))
        return true;
    return point_on_segment(c, a, b) || point_on_segment(d, a, b) || point_on_segment(a, c, d) ||
           point_on_segment(b, c, d);
}

inline bool valid_inference_polygon(const InferenceRoiConfig &roi)
{
    if (roi.polygon.size() < 3 || roi.polygon.size() > 64)
        return false;
    double twice_area = 0.0;
    for (size_t i = 0; i < roi.polygon.size(); ++i)
    {
        const auto &a = roi.polygon[i];
        const auto &b = roi.polygon[(i + 1) % roi.polygon.size()];
        if (!std::isfinite(a.first) || !std::isfinite(a.second) || a.first < 0.0 || a.first > 1.0 || a.second < 0.0 ||
            a.second > 1.0)
            return false;
        if (std::abs(a.first - b.first) <= 1e-10 && std::abs(a.second - b.second) <= 1e-10)
            return false;
        twice_area += a.first * b.second - b.first * a.second;
    }
    if (std::abs(twice_area) <= 1e-8)
        return false;
    const size_t count = roi.polygon.size();
    for (size_t i = 0; i < count; ++i)
        for (size_t j = i + 1; j < count; ++j)
        {
            if (j == i + 1 || (i == 0 && j == count - 1))
                continue;
            if (segments_intersect(roi.polygon[i], roi.polygon[(i + 1) % count], roi.polygon[j],
                                   roi.polygon[(j + 1) % count]))
                return false;
        }
    return true;
}

} // namespace vision
