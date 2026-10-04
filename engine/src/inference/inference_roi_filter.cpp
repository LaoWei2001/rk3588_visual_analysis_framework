#include "inference_roi_filter.h"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <utility>

namespace
{
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

void filter_results_to_inference_roi(std::vector<AlgoResult> &results, const InferenceRoiConfig &roi,
                                     int canonical_width, int canonical_height)
{
    if (!roi.has_polygon() || canonical_width <= 0 || canonical_height <= 0)
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
