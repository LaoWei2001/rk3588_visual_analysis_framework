#include "inference_roi_filter.h"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <utility>

namespace
{
cv::Rect scale_roi(const cv::Rect &source_roi, int source_width, int source_height, int target_width, int target_height)
{
    if (source_width <= 0 || source_height <= 0 || target_width <= 0 || target_height <= 0)
        return {};

    const int left = std::max(0, std::min(target_width, static_cast<int>(std::floor(static_cast<double>(source_roi.x) *
                                                                                    target_width / source_width))));
    const int top = std::max(0, std::min(target_height, static_cast<int>(std::floor(static_cast<double>(source_roi.y) *
                                                                                    target_height / source_height))));
    const int right = std::max(
        left, std::min(target_width, static_cast<int>(std::ceil(static_cast<double>(source_roi.x + source_roi.width) *
                                                                target_width / source_width))));
    const int bottom = std::max(
        top, std::min(target_height, static_cast<int>(std::ceil(static_cast<double>(source_roi.y + source_roi.height) *
                                                                target_height / source_height))));
    return cv::Rect(left, top, right - left, bottom - top);
}

void merge_result_masks(std::vector<AlgoResult> &results, cv::Mat &merged_mask)
{
    for (AlgoResult &result : results)
    {
        if (result.boxMask.empty())
            continue;
        if (merged_mask.empty())
            merged_mask = std::move(result.boxMask);
        else if (merged_mask.size() == result.boxMask.size() && merged_mask.type() == result.boxMask.type())
        {
            cv::Mat foreground;
            cv::compare(result.boxMask, 0, foreground, cv::CMP_NE);
            result.boxMask.copyTo(merged_mask, foreground);
            result.boxMask.release();
        }
        else
            result.boxMask.release();
    }
}
} // namespace

void filter_results_to_inference_roi(std::vector<AlgoResult> &results, const cv::Rect &source_roi, int source_width,
                                     int source_height, int canonical_width, int canonical_height)
{
    const cv::Rect canonical_roi =
        scale_roi(source_roi, source_width, source_height, canonical_width, canonical_height);
    if (canonical_roi.empty())
    {
        results.clear();
        return;
    }

    cv::Mat merged_mask;
    merge_result_masks(results, merged_mask);

    results.erase(std::remove_if(results.begin(), results.end(),
                                 [&](const AlgoResult &result) {
                                     return result.box.empty() || !canonical_roi.contains(result.box_center());
                                 }),
                  results.end());

    if (results.empty() || merged_mask.empty())
        return;

    const cv::Rect mask_roi = scale_roi(source_roi, source_width, source_height, merged_mask.cols, merged_mask.rows);
    cv::Mat clipped_mask = cv::Mat::zeros(merged_mask.size(), merged_mask.type());
    if (!mask_roi.empty())
        merged_mask(mask_roi).copyTo(clipped_mask(mask_roi));
    results.front().boxMask = std::move(clipped_mask);
}

void filter_results_to_inference_roi(std::vector<AlgoResult> &results, const InferenceRoiConfig &roi, int source_width,
                                     int source_height, int canonical_width, int canonical_height)
{
    if (!roi.has_polygon())
    {
        const cv::Rect source_roi(static_cast<int>(std::floor(roi.x * source_width)),
                                  static_cast<int>(std::floor(roi.y * source_height)),
                                  static_cast<int>(std::ceil((roi.x + roi.width) * source_width)) -
                                      static_cast<int>(std::floor(roi.x * source_width)),
                                  static_cast<int>(std::ceil((roi.y + roi.height) * source_height)) -
                                      static_cast<int>(std::floor(roi.y * source_height)));
        filter_results_to_inference_roi(results, source_roi, source_width, source_height, canonical_width,
                                        canonical_height);
        return;
    }
    if (canonical_width <= 0 || canonical_height <= 0)
    {
        results.clear();
        return;
    }

    std::vector<cv::Point2f> canonical_polygon;
    canonical_polygon.reserve(roi.polygon.size());
    for (const auto &point : roi.polygon)
        canonical_polygon.emplace_back(static_cast<float>(point.first * canonical_width),
                                       static_cast<float>(point.second * canonical_height));

    cv::Mat merged_mask;
    merge_result_masks(results, merged_mask);
    results.erase(std::remove_if(results.begin(), results.end(),
                                 [&](const AlgoResult &result) {
                                     return result.box.empty() ||
                                            cv::pointPolygonTest(canonical_polygon, result.box_center(), false) < 0.0;
                                 }),
                  results.end());
    if (results.empty() || merged_mask.empty())
        return;

    std::vector<cv::Point> mask_polygon;
    mask_polygon.reserve(roi.polygon.size());
    for (const auto &point : roi.polygon)
        mask_polygon.emplace_back(
            std::max(0, std::min(merged_mask.cols - 1, static_cast<int>(std::lround(point.first * merged_mask.cols)))),
            std::max(0,
                     std::min(merged_mask.rows - 1, static_cast<int>(std::lround(point.second * merged_mask.rows)))));
    cv::Mat keep = cv::Mat::zeros(merged_mask.size(), CV_8UC1);
    cv::fillPoly(keep, std::vector<std::vector<cv::Point>>{mask_polygon}, cv::Scalar(255));
    cv::Mat clipped = cv::Mat::zeros(merged_mask.size(), merged_mask.type());
    merged_mask.copyTo(clipped, keep);
    results.front().boxMask = std::move(clipped);
}
