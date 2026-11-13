#pragma once

#include <algorithm>
#include <opencv2/core.hpp>
#include <vector>

namespace zip_depth
{
// ZipDepth.infer_image uses endpoint-aligned bilinear interpolation. OpenCV
// resize uses half-pixel coordinates instead. Cache the horizontal coordinates
// per worker, and write directly into the destination (including strided ROIs).
inline void resize_depth(const cv::Mat &source, cv::Mat &destination, cv::Size size)
{
    CV_Assert(!source.empty() && source.type() == CV_32FC1 && size.width > 0 && size.height > 0);
    if (source.size() == size)
    {
        source.copyTo(destination);
        return;
    }
    struct Columns
    {
        int source_width = 0, target_width = 0;
        std::vector<int> left, right;
        std::vector<float> weight;
    };
    static thread_local Columns columns;
    if (columns.source_width != source.cols || columns.target_width != size.width)
    {
        columns.source_width = source.cols;
        columns.target_width = size.width;
        columns.left.resize(size.width);
        columns.right.resize(size.width);
        columns.weight.resize(size.width);
        const double scale = size.width == 1 ? 0.0 : double(source.cols - 1) / (size.width - 1);
        for (int x = 0; x < size.width; ++x)
        {
            const float position = static_cast<float>(x * scale);
            const int left = std::min(static_cast<int>(position), source.cols - 1);
            columns.left[x] = left;
            columns.right[x] = std::min(left + 1, source.cols - 1);
            columns.weight[x] = position - left;
        }
    }
    destination.create(size, CV_32FC1);
    const double scale = size.height == 1 ? 0.0 : double(source.rows - 1) / (size.height - 1);
    for (int y = 0; y < size.height; ++y)
    {
        const float position = static_cast<float>(y * scale);
        const int top = std::min(static_cast<int>(position), source.rows - 1);
        const float wy = position - top;
        const float *a = source.ptr<float>(top), *b = source.ptr<float>(std::min(top + 1, source.rows - 1));
        float *out = destination.ptr<float>(y);
        for (int x = 0; x < size.width; ++x)
        {
            const int l = columns.left[x], r = columns.right[x];
            const float wx = columns.weight[x];
            out[x] = (a[l] * (1 - wx) + a[r] * wx) * (1 - wy) + (b[l] * (1 - wx) + b[r] * wx) * wy;
        }
    }
}
} // namespace zip_depth
