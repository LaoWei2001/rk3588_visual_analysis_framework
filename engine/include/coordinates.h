/** @file coordinates.h
 * @brief 统一业务坐标：ROI、检测结果与绘图共用的画布尺寸。
 * 功能索引与用法见同目录 README.md。
 */
#pragma once

#include <opencv2/core.hpp>

/* 完整视频的稳定业务画布；与模型输入尺寸、原图分辨率独立。
 * 像素距离/面积阈值、ROI、跟踪和绘图均以此为基准。不可随模型热切换改变。 */
namespace business_coordinates
{
constexpr int WIDTH = 640;
constexpr int HEIGHT = 640;

/** @brief 返回固定业务尺寸 640×640，与模型和原图尺寸无关。
 * 示例：
 * @code
 * cv::Size size = business_coordinates::size();
 * @endcode
 */
inline cv::Size size()
{
    return cv::Size(WIDTH, HEIGHT);
}
/** @brief 返回业务中心 (320,320)，不获取图像。
 * 示例：
 * @code
 * cv::Point center = business_coordinates::center();
 * @endcode
 */
inline cv::Point center()
{
    return cv::Point(WIDTH / 2, HEIGHT / 2);
}
} // namespace business_coordinates
