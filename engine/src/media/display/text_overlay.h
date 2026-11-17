#pragma once
#include <opencv2/core.hpp>
#include <string>

/**
 * @brief 高性能统一文字绘制出口。
 *
 * 用 FreeType 生成灰度字形蒙版；重复出现的稳定整行文本会晋升到线程内有界 LRU，
 * 包含实时数字、每帧变化的文本由 Unicode 字形缓存拼装，数值变化不再触发整行
 * FreeType 重新栅格化。命中时只在文字
 * 实际包围框内按二值蒙版着色。缓存阶段将不同
 * OpenCV/FreeType 实现输出的字形值统一归一化为 0/255；开启重影时，膨胀后的重影蒙版也只生成
 * 一次。缓存不包含颜色和坐标，因此同一段文字可在不同位置、颜色间复用。
 *
 * @param thickness       逻辑粗细：<=1 为填充字；>=2 为填充字加同色笔画。
 * @param shadow_enabled  true=绘制重影/外描边；false=只绘制前景文字。
 * @param shadow_color    重影 BGR 颜色。
 * @param shadow_width    重影膨胀宽度，内部限制为 1~8 像素。
 */
bool draw_text_unicode_cached(cv::Mat &img, const std::string &utf8, cv::Point org, int font_height_px,
                              const cv::Scalar &color, int thickness, bool shadow_enabled,
                              const cv::Scalar &shadow_color, int shadow_width);
