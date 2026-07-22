/**
 * @file text_overlay.h
 * @brief 画面 UTF-8/中文 文本叠加 (基于 OpenCV freetype 模块)
 *
 * OpenCV 自带的 cv::putText 只支持 Hershey 矢量字体, 无中文字形。本模块用 OpenCV 的 freetype
 * 模块 + 一个含 CJK 字形的 TTF/TTC 字体, 统一渲染任意 UTF-8 文本(中文+英文)。freetype 为必需
 * (CMake 已强制, 缺失直接报错); 字体加载失败则文字不绘制并打印错误, 不再回退 Hershey。
 *
 * 字体查找顺序(首个可加载者生效):
 *   1) 环境变量 RK_OVERLAY_FONT 指定的路径
 *   2) ./assets/fonts/overlay.{ttf,ttc,otf}   (随程序打包, 可自定义)
 *   3) /usr/share/fonts/truetype/wqy/wqy-zenhei.ttc   (文泉驿正黑, 多数板子自带)
 *   4) /usr/share/fonts/truetype/wqy/wqy-microhei.ttc
 */
#pragma once

#include <opencv2/opencv.hpp>
#include <string>

/** @brief 中文/UTF-8 文本渲染是否可用(freetype 模块已编译 且 成功加载到字体)。
 *  逻辑层可据此决定显示中文还是 ASCII 短标签。首次调用会惰性加载字体(线程安全)。 */
bool text_overlay_available();

/**
 * @brief 在 img 上绘制 UTF-8 文本(支持中文)。
 * @param org            文本左下角基线点(与 cv::putText 的 bottomLeftOrigin 行为一致, 便于直接替换)
 * @param font_height_px 字符像素高度
 * @param color          BGR 颜色
 * @param thickness      <0 填充字形(推荐), >0 为描边粗细
 * @return true=已用中文字体绘制成功; false=不可用 → 调用方应回退 cv::putText
 */
bool draw_text_unicode(cv::InputOutputArray img, const std::string &utf8, cv::Point org,
                       int font_height_px, const cv::Scalar &color, int thickness);