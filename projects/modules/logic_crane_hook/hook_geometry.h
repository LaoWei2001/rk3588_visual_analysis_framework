#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <vector>

namespace crane_safety
{

struct HookEdges
{
    bool left = false, right = false, top = false, bottom = false;
    bool valid = false;
    bool truncated() const { return left || right || top || bottom; }
    bool complete() const { return valid && !truncated(); }
};

HookEdges hook_edges(const cv::Rect &box, const cv::Size &frame, int margin);

struct HookCalibrationPoint
{
    double width = 0.0, height = 0.0, center_x = 0.0, center_y = 0.0;
};

struct HookCalibration
{
    std::vector<HookCalibrationPoint> points;
    double min_height = 0.0, max_height = 0.0;
    int accepted = 0, rejected = 0;
    std::string error;
    bool lookup(double height, HookCalibrationPoint *point) const;
};

HookCalibration load_hook_calibration(const std::string &path, double height_bin,
                                     const cv::Size &frame, int margin);

struct HookGeometry
{
    bool valid = false;
    bool estimated = false;
    bool truncated = false;
    bool calibrated = false;
    cv::Point center{-1, -1};
    cv::Point safe_center{-1, -1};
    std::string status;
};

HookGeometry measure_hook(const cv::Rect &box, const cv::Size &frame, int margin,
                          const HookCalibration &calibration, bool require_calibration);

} // namespace crane_safety
