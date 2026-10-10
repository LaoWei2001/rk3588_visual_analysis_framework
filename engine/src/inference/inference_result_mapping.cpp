#include "inference/inference_result_mapping.h"
#include <coordinates.h>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

void map_results_to_business_frame(int source_width, int source_height, const InferenceRoiTransform &transform,
                                   int model_width, int model_height, std::vector<AlgoResult> &results)
{
    if (results.empty() || source_width <= 0 || source_height <= 0 || model_width <= 0 || model_height <= 0)
        return;

    const cv::Rect visible_source = transform.source_rect & cv::Rect(0, 0, source_width, source_height);
    const cv::Rect content = transform.model_content_rect & cv::Rect(0, 0, model_width, model_height);
    if (visible_source.empty() || content.empty())
        return;

    constexpr int width = business_coordinates::WIDTH;
    constexpr int height = business_coordinates::HEIGHT;
    const cv::Rect bounds(0, 0, width, height);
    const bool identity = model_width == width && model_height == height &&
                          visible_source == cv::Rect(0, 0, source_width, source_height) && content == bounds;
    if (identity && std::all_of(results.begin(), results.end(), [](const AlgoResult &result) {
            return result.boxMask.empty() || result.boxMask.size() == business_coordinates::size();
        }))
    {
        // 最常用的 640 整帧检测/姿态/分割：没有矩阵构造、掩码区域计算或浮点缩放。
        for (AlgoResult &result : results)
        {
            const bool center_in_content = bounds.contains(result.box_center());
            result.box &= bounds;
            if (!center_in_content)
                result.box = cv::Rect();
            for (cv::Point2f &point : result.keypoints)
            {
                if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0.0f || point.y < 0.0f)
                    continue;
                point.x = std::min(static_cast<float>(width - 1), point.x);
                point.y = std::min(static_cast<float>(height - 1), point.y);
            }
        }
        return;
    }
    const float scale_x =
        static_cast<float>(visible_source.width) * width / (static_cast<float>(content.width) * source_width);
    const float scale_y =
        static_cast<float>(visible_source.height) * height / (static_cast<float>(content.height) * source_height);
    const float offset_x = static_cast<float>(visible_source.x) * width / source_width - content.x * scale_x;
    const float offset_y = static_cast<float>(visible_source.y) * height / source_height - content.y * scale_y;

    // 检测/姿态热路径无需计算掩码区域；第一张非空掩码才需要它。
    cv::Rect mask_roi;
    bool mask_roi_ready = false;
    // 若多个结果共享同一整帧掩码，同一次映射只转换一次。
    const unsigned char *last_mask_data = nullptr;
    size_t last_mask_step = 0;
    cv::Size last_mask_size;
    int last_mask_type = -1;
    cv::Mat last_mapped_mask;

    for (AlgoResult &result : results)
    {
        const cv::Point model_center = result.box_center();
        const bool center_in_content = content.contains(model_center);
        if (identity)
            result.box &= bounds;
        else
        {
            const float left = offset_x + result.box.x * scale_x;
            const float top = offset_y + result.box.y * scale_y;
            const float right = offset_x + (result.box.x + result.box.width) * scale_x;
            const float bottom = offset_y + (result.box.y + result.box.height) * scale_y;
            const int x = static_cast<int>(std::floor(left));
            const int y = static_cast<int>(std::floor(top));
            result.box = cv::Rect(x, y, std::max(0, static_cast<int>(std::ceil(right)) - x),
                                  std::max(0, static_cast<int>(std::ceil(bottom)) - y)) &
                         bounds;
        }
        if (!center_in_content)
            result.box = cv::Rect();

        for (cv::Point2f &point : result.keypoints)
        {
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || point.x < 0.0f || point.y < 0.0f)
                continue;
            if (!identity)
            {
                point.x = offset_x + point.x * scale_x;
                point.y = offset_y + point.y * scale_y;
            }
            point.x = std::max(0.0f, std::min(static_cast<float>(width - 1), point.x));
            point.y = std::max(0.0f, std::min(static_cast<float>(height - 1), point.y));
        }

        if (result.boxMask.empty() || (identity && result.boxMask.size() == business_coordinates::size()))
            continue; // 常用 640 整帧路径：不分配、不复制、不缩放掩码。

        const cv::Mat &mask = result.boxMask;
        if (mask.data == last_mask_data && mask.step[0] == last_mask_step && mask.size() == last_mask_size &&
            mask.type() == last_mask_type)
        {
            result.boxMask = last_mapped_mask;
            continue;
        }
        if (!mask_roi_ready)
        {
            const int x0 = std::max(0, std::min(width, static_cast<int>(std::lround(offset_x + content.x * scale_x))));
            const int y0 = std::max(0, std::min(height, static_cast<int>(std::lround(offset_y + content.y * scale_y))));
            const int x1 = std::max(
                x0, std::min(width, static_cast<int>(std::lround(offset_x + (content.x + content.width) * scale_x))));
            const int y1 = std::max(
                y0, std::min(height, static_cast<int>(std::lround(offset_y + (content.y + content.height) * scale_y))));
            mask_roi = cv::Rect(x0, y0, x1 - x0, y1 - y0);
            mask_roi_ready = true;
        }
        const float mask_scale_x = static_cast<float>(mask.cols) / model_width;
        const float mask_scale_y = static_cast<float>(mask.rows) / model_height;
        const cv::Rect mask_content(static_cast<int>(std::floor(content.x * mask_scale_x)),
                                    static_cast<int>(std::floor(content.y * mask_scale_y)),
                                    std::max(1, static_cast<int>(std::ceil(content.width * mask_scale_x))),
                                    std::max(1, static_cast<int>(std::ceil(content.height * mask_scale_y))));
        const cv::Rect clipped_content = mask_content & cv::Rect(0, 0, mask.cols, mask.rows);
        last_mask_data = mask.data;
        last_mask_step = mask.step[0];
        last_mask_size = mask.size();
        last_mask_type = mask.type();
        cv::Mat mapped;
        if (!mask_roi.empty() && !clipped_content.empty())
        {
            if (mask_roi == bounds)
                cv::resize(mask(clipped_content), mapped, business_coordinates::size(), 0.0, 0.0, cv::INTER_NEAREST);
            else
            {
                mapped = cv::Mat::zeros(height, width, mask.type());
                cv::Mat destination = mapped(mask_roi);
                cv::resize(mask(clipped_content), destination, mask_roi.size(), 0.0, 0.0, cv::INTER_NEAREST);
            }
        }
        result.boxMask = mapped;
        last_mapped_mask = std::move(mapped);
    }
}
