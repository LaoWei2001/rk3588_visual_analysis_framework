/** @file drawing.h
 * @brief 通用绘图：提交矩形、线、文字和多边形；需要直接操作像素时使用 display_canvas()。
 * 功能索引与用法见同目录 README.md。
 */
#pragma once

#include <opencv2/core.hpp>
#include <types.h>

struct ChannelContext;

/*======================== 绘制辅助函数 ========================*/
/* 矩形/圆: thickness=-1(负数) = 填充; alpha<1 = 半透明叠加(目标/画面可透出来, 适合高亮报警区)。
 * 例: draw_rect(ctx, zone, 红, -1, 0.3)  → 半透明红色块盖住 zone, 区域内的人仍看得见。 */
void draw_rect(ChannelContext *ctx, const cv::Rect &rect, const cv::Scalar &color = cv::Scalar(0, 255, 0),
               int thickness = 2, double alpha = 1.0, DrawCommand::Target target = DrawCommand::ALL);

void draw_circle(ChannelContext *ctx, const cv::Point &center, int radius,
                 const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2, double alpha = 1.0,
                 DrawCommand::Target target = DrawCommand::ALL);

void draw_line(ChannelContext *ctx, const cv::Point &pt1, const cv::Point &pt2,
               const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2,
               DrawCommand::Target target = DrawCommand::ALL);

/* 统一文字绘制接口：
 * - thickness: <=1 普通填充字；>=2 在填充字上增加同色笔画。
 * - shadow_enabled: 是否启用深色重影/外描边；打开后由底层缓存一次字形蒙版并膨胀，
 *   不会把同一段文字重复栅格化两次。
 * - target: DISPLAY / IMAGE / VIDEO；MEDIA 表示图片+视频，ALL 表示三者。
 * 报警大字示例: draw_text(ctx,"报警",pos,红,1.0,4)。 */
void draw_text(ChannelContext *ctx, const char *text, const cv::Point &pos,
               const cv::Scalar &color = cv::Scalar(255, 255, 255), double font_scale = 0.6, int thickness = 1,
               DrawCommand::Target target = DrawCommand::ALL, bool shadow_enabled = false,
               const cv::Scalar &shadow_color = cv::Scalar(0, 0, 0), int shadow_width = 2);

/**
 * @brief 按单通道 8-bit 蒙版对 display_canvas() 做批量颜色融合。
 *
 * 底层使用 OpenCV SIMD/NEON 路径和线程局部复用缓冲，业务模块不应再写
 * 逐像素 C++ 双层循环。mask 与业务画布尺寸不同时会自动最近邻缩放。
 */
bool blend_display_mask(ChannelContext *ctx, const cv::Mat &mask, const cv::Scalar &color, double alpha);

/* 折线: 把一串点连成线; alpha<1 时半透明叠加(可让画面/手透过来, 看着更清楚)。
 * 比逐段 draw_line 更高效(一条指令), 且自交叠处不会因半透明而叠暗。 */
void draw_polyline(ChannelContext *ctx, const std::vector<cv::Point> &points,
                   const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2, double alpha = 1.0,
                   bool closed = false, DrawCommand::Target target = DrawCommand::ALL);

/* 填充多边形(实心色块); alpha<1 半透明叠加 —— 给一块 ROI/区域铺半透明底色高亮最常用。
 * 顶点为业务坐标系(640×640)(与 ROI/检测框同坐标系); 少于 3 个点不绘制。
 * 例: draw_poly_filled(ctx, *ctx->roi_polygon_at(0), 红, 0.3)  → 把首个 ROI 铺成半透明红。 */
void draw_poly_filled(ChannelContext *ctx, const std::vector<cv::Point> &points,
                      const cv::Scalar &color = cv::Scalar(0, 255, 0), double alpha = 0.3,
                      DrawCommand::Target target = DrawCommand::ALL);

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
