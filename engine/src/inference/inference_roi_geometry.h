#pragma once

#include <opencv2/core.hpp>
#include <string>
#include <utility>
#include <vector>

/* 一次 ROI 推理的完整几何变换：source_rect 从原图取景，内容缩放后落入
 * model_content_rect。后者小于模型画布时，其余位置必须填黑。 */
struct InferenceRoiTransform
{
    cv::Rect source_rect;
    cv::Rect model_content_rect;

    bool valid() const
    {
        return !source_rect.empty() && !model_content_rect.empty();
    }
    bool needs_padding(int model_width, int model_height) const
    {
        return model_content_rect != cv::Rect(0, 0, model_width, model_height);
    }
};

InferenceRoiTransform make_inference_roi_transform(const cv::Rect &selection, int source_width, int source_height,
                                                   int model_width, int model_height, const std::string &resize_mode,
                                                   bool align_yuv420);

/* 按 transform 生成真正送给模型的 BGR 画布。mask_polygon 使用完整原图的
 * 0~1 归一化坐标；非空时，多边形外（包括 letterbox 区域）严格填 0。 */
cv::Mat make_inference_roi_input(const cv::Mat &source, const InferenceRoiTransform &transform, int model_width,
                                 int model_height,
                                 const std::vector<std::pair<double, double>> *mask_polygon = nullptr);
