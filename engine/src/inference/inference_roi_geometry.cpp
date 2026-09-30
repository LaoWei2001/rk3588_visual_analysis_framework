#include "inference_roi_geometry.h"

#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>

namespace
{
cv::Rect fit_content(int source_width, int source_height, int model_width, int model_height)
{
    const double scale =
        std::min(static_cast<double>(model_width) / source_width, static_cast<double>(model_height) / source_height);
    const int width = std::max(1, std::min(model_width, static_cast<int>(std::lround(source_width * scale))));
    const int height = std::max(1, std::min(model_height, static_cast<int>(std::lround(source_height * scale))));
    return cv::Rect((model_width - width) / 2, (model_height - height) / 2, width, height);
}

cv::Rect align_crop(const cv::Rect &input, const cv::Rect &bounds, bool align_yuv420)
{
    cv::Rect result = input & bounds;
    if (!align_yuv420 || result.empty())
        return result;
    int left = result.x & ~1;
    int top = result.y & ~1;
    int right = std::min(bounds.width, (result.x + result.width + 1) & ~1);
    int bottom = std::min(bounds.height, (result.y + result.height + 1) & ~1);
    if (right <= left)
        right = std::min(bounds.width, left + 2);
    if (bottom <= top)
        bottom = std::min(bounds.height, top + 2);
    return cv::Rect(left, top, right - left, bottom - top);
}
} // namespace

cv::Mat make_inference_roi_input(const cv::Mat &source, const InferenceRoiTransform &transform, int model_width,
                                 int model_height, const std::vector<std::pair<double, double>> *mask_polygon)
{
    if (source.empty() || !transform.valid() || model_width <= 0 || model_height <= 0)
        return {};
    const cv::Rect clipped = transform.source_rect & cv::Rect(0, 0, source.cols, source.rows);
    const cv::Rect content = transform.model_content_rect & cv::Rect(0, 0, model_width, model_height);
    if (clipped.empty() || content.empty() || clipped != transform.source_rect || content != transform.model_content_rect)
        return {};

    cv::Mat output = cv::Mat::zeros(model_height, model_width, source.type());
    cv::resize(source(clipped), output(content), content.size(), 0.0, 0.0, cv::INTER_LINEAR);
    if (!mask_polygon || mask_polygon->size() < 3)
        return output;

    std::vector<cv::Point> points;
    points.reserve(mask_polygon->size());
    for (const auto &point : *mask_polygon)
    {
        const double source_x = point.first * source.cols;
        const double source_y = point.second * source.rows;
        const int model_x = static_cast<int>(std::lround(
            content.x + (source_x - clipped.x) * static_cast<double>(content.width) / clipped.width));
        const int model_y = static_cast<int>(std::lround(
            content.y + (source_y - clipped.y) * static_cast<double>(content.height) / clipped.height));
        points.emplace_back(model_x, model_y);
    }
    cv::Mat mask = cv::Mat::zeros(model_height, model_width, CV_8UC1);
    const std::vector<std::vector<cv::Point>> contours{points};
    cv::fillPoly(mask, contours, cv::Scalar(255), cv::LINE_8);
    output.setTo(cv::Scalar::all(0), mask == 0);
    return output;
}

InferenceRoiTransform make_inference_roi_transform(const cv::Rect &selection, int source_width, int source_height,
                                                   int model_width, int model_height, const std::string &resize_mode,
                                                   bool align_yuv420)
{
    InferenceRoiTransform transform;
    if (source_width <= 0 || source_height <= 0 || model_width <= 0 || model_height <= 0)
        return transform;
    const cv::Rect bounds(0, 0, source_width, source_height);
    cv::Rect source = selection & bounds;
    if (source.empty())
        return transform;

    if (resize_mode == "expand")
    {
        const double target_ratio = static_cast<double>(model_width) / model_height;
        double width = source.width;
        double height = source.height;
        if (width / height < target_ratio)
            width = height * target_ratio;
        else
            height = width / target_ratio;

        /* 当目标比例在某一方向超出原图时，只截断该方向；另一方向仍保留
         * 最小取景范围，随后由 model_content_rect 补边。旧逻辑在任一方向
         * 超界时直接退化为整帧，导致稍大的宽 ROI 突然失去局部放大效果。 */
        width = std::min(width, static_cast<double>(source_width));
        height = std::min(height, static_cast<double>(source_height));
        const double center_x = source.x + source.width * 0.5;
        const double center_y = source.y + source.height * 0.5;
        int left = static_cast<int>(std::floor(center_x - width * 0.5));
        int top = static_cast<int>(std::floor(center_y - height * 0.5));
        int right = left + static_cast<int>(std::ceil(width));
        int bottom = top + static_cast<int>(std::ceil(height));
        if (left < 0)
        {
            right -= left;
            left = 0;
        }
        if (top < 0)
        {
            bottom -= top;
            top = 0;
        }
        if (right > source_width)
        {
            left -= right - source_width;
            right = source_width;
        }
        if (bottom > source_height)
        {
            top -= bottom - source_height;
            bottom = source_height;
        }
        source = cv::Rect(std::max(0, left), std::max(0, top), right - std::max(0, left), bottom - std::max(0, top));
    }

    source = align_crop(source, bounds, align_yuv420);
    if (source.empty())
        return transform;
    transform.source_rect = source;
    transform.model_content_rect = (resize_mode == "stretch")
                                       ? cv::Rect(0, 0, model_width, model_height)
                                       : fit_content(source.width, source.height, model_width, model_height);
    return transform;
}
