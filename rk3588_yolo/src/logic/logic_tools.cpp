/**
 * @file logic_tools.cpp
 * @brief 业务逻辑工具函数实现
 */

#include "logic_tools.h"
#include <opencv2/opencv.hpp>
#include <vector>

namespace
{
static void clip_points(std::vector<cv::Point> &points, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;

    for (auto &p : points)
    {
        if (p.x < 0)
            p.x = 0;
        else if (p.x >= width)
            p.x = width - 1;

        if (p.y < 0)
            p.y = 0;
        else if (p.y >= height)
            p.y = height - 1;
    }
}

static cv::Mat build_roi_mask(const cv::Size &size, const std::vector<cv::Point> &points)
{
    cv::Mat mask(size, CV_8UC1, cv::Scalar(0));
    if (points.size() < 3)
        return mask;

    std::vector<std::vector<cv::Point>> polys(1, points);
    cv::fillPoly(mask, polys, cv::Scalar(255));
    return mask;
}

static int dominant_hue(const cv::Mat &hsv, const cv::Mat &valid_mask)
{
    cv::Mat hue;
    cv::extractChannel(hsv, hue, 0);

    int hist_size = 180;
    float range[] = {0.f, 180.f};
    const float *ranges[] = {range};
    cv::Mat hist;
    cv::calcHist(&hue, 1, nullptr, valid_mask, hist, 1, &hist_size, ranges, true, false);

    double max_val = 0.0;
    cv::Point max_loc;
    cv::minMaxLoc(hist, nullptr, &max_val, nullptr, &max_loc);
    return max_loc.y;
}

static cv::Mat remove_small_components(const cv::Mat &mask_u8, int min_area)
{
    cv::Mat labels, stats, centroids;
    int n = cv::connectedComponentsWithStats(mask_u8, labels, stats, centroids, 8, CV_32S);

    cv::Mat out(mask_u8.size(), CV_8UC1, cv::Scalar(0));
    for (int i = 1; i < n; ++i)
    {
        int area = stats.at<int>(i, cv::CC_STAT_AREA);
        if (area >= min_area)
        {
            out.setTo(255, labels == i);
        }
    }
    return out;
}
} // namespace

bool logic_roll_compute_occupancy(const cv::Mat &bgr,
                                  const std::vector<cv::Point> &roi_points,
                                  int sat_min,
                                  int val_min,
                                  int hue_tol,
                                  int min_area,
                                  double &ratio,
                                  cv::Mat &occupancy_mask,
                                  int &bg_hue)
{
    ratio = 0.0;
    occupancy_mask.release();
    bg_hue = 0;

    if (bgr.empty() || roi_points.size() < 3)
        return false;

    std::vector<cv::Point> clipped = roi_points;
    clip_points(clipped, bgr.cols, bgr.rows);
    cv::Mat roi_mask = build_roi_mask(bgr.size(), clipped);
    int roi_pixels = cv::countNonZero(roi_mask);
    if (roi_pixels <= 0)
        return false;

    cv::Mat hsv;
    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);

    cv::Mat channels[3];
    cv::split(hsv, channels);
    cv::Mat hue = channels[0];
    cv::Mat sat = channels[1];
    cv::Mat val = channels[2];

    cv::Mat sat_ok, val_ok, roi_u8;
    cv::compare(sat, sat_min, sat_ok, cv::CMP_GE);
    cv::compare(val, val_min, val_ok, cv::CMP_GE);
    cv::compare(roi_mask, 0, roi_u8, cv::CMP_GT);

    cv::Mat strong_color;
    cv::bitwise_and(roi_u8, sat_ok, strong_color);
    cv::bitwise_and(strong_color, val_ok, strong_color);

    cv::Mat ref_mask = (cv::countNonZero(strong_color) < 100) ? roi_u8 : strong_color;
    bg_hue = dominant_hue(hsv, ref_mask);

    cv::Mat h16;
    hue.convertTo(h16, CV_16S);
    cv::Mat diff1, diff2, circular_dist;
    cv::absdiff(h16, cv::Scalar(bg_hue), diff1);
    cv::absdiff(diff1, cv::Scalar(180), diff2);
    cv::min(diff1, diff2, circular_dist);

    cv::Mat hue_ok;
    cv::compare(circular_dist, hue_tol, hue_ok, cv::CMP_LE);

    cv::Mat background_mask(bgr.size(), CV_8UC1, cv::Scalar(0));
    cv::bitwise_and(roi_u8, sat_ok, background_mask);
    cv::bitwise_and(background_mask, val_ok, background_mask);
    cv::bitwise_and(background_mask, hue_ok, background_mask);

    cv::Mat not_background;
    cv::bitwise_not(background_mask, not_background);
    cv::bitwise_and(roi_u8, not_background, occupancy_mask);

    cv::Mat kernel = cv::Mat::ones(3, 3, CV_8U);
    cv::morphologyEx(occupancy_mask, occupancy_mask, cv::MORPH_OPEN, kernel, cv::Point(-1, -1), 1);
    cv::morphologyEx(occupancy_mask, occupancy_mask, cv::MORPH_CLOSE, kernel, cv::Point(-1, -1), 1);
    occupancy_mask = remove_small_components(occupancy_mask, min_area);

    int occ_pixels = cv::countNonZero(occupancy_mask);
    ratio = static_cast<double>(occ_pixels) / static_cast<double>(roi_pixels);
    return true;
}
