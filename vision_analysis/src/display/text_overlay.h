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
 *  逻辑层可据此决定显示中文还是 ASCII 短标签。首次调用会为当前线程惰性加载
 *  一个独立字体渲染器，避免多路显示被全局 FreeType 锁串行化。 */
bool text_overlay_available();

/** @brief 使用当前线程的 FreeType face 测量 UTF-8 文字边界。
 * @param[out] size     包围文字的宽高
 * @param[out] baseline 基线到文字最底部的距离
 * @return true=测量成功; false=文字为空或字体不可用
 */
bool measure_text_unicode(const std::string &utf8, int font_height_px, int thickness, cv::Size &size, int &baseline);

/**
 * @brief 在 img 上绘制 UTF-8 文本(支持中文)。
 * @param org            文本左下角基线点(与 cv::putText 的 bottomLeftOrigin 行为一致, 便于直接替换)
 * @param font_height_px 字符像素高度
 * @param color          BGR 颜色
 * @param thickness      <0 填充字形(推荐), >0 为描边粗细
 * 同一线程重用自己的 FreeType face，不同线程之间可并行调用。
 * @return true=已用中文字体绘制成功; false=文字为空或字体不可用，由调用方处理；统一显示路径不回退
 */
bool draw_text_unicode(cv::InputOutputArray img, const std::string &utf8, cv::Point org, int font_height_px,
                       const cv::Scalar &color, int thickness);

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
