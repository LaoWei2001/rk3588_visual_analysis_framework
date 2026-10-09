#pragma once

#include <opencv2/core.hpp>

/* 完整视频的稳定业务画布；与模型输入尺寸、原图分辨率独立。
 * 像素距离/面积阈值、ROI、跟踪和绘图均以此为基准。不可随模型热切换改变。 */
namespace business_coordinates
{
constexpr int WIDTH = 640;
constexpr int HEIGHT = 640;

inline cv::Size size() { return cv::Size(WIDTH, HEIGHT); }
inline cv::Point center() { return cv::Point(WIDTH / 2, HEIGHT / 2); }
} // namespace business_coordinates
