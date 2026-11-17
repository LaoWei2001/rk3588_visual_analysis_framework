/** @file drawing.h
 * @brief 在视频画面或自己的图片上画框、文字、线和区域。
 * 带 ctx 的绘图函数在通道处理函数里使用，位置按 640×640 图片填写。
 * 颜色写作 cv::Scalar(蓝, 绿, 红)，每项 0~255；(0,0,255) 是红色。
 * target 可选 DrawCommand::DISPLAY（实时画面）、IMAGE（生成的图片）、VIDEO（生成的视频）、
 * MEDIA（图片和视频）、ALL（全部，默认）；按需生成这些图片或视频时才会出现对应绘图。
 * blend_mask、draw_text_unicode 等直接修改图片的函数，使用那张图片自身的像素坐标。
 */
#pragma once

#include <opencv2/core.hpp>
#include <types.h>

struct ChannelContext;

/*======================== 绘制辅助函数 ========================*/
/** @brief 在画面上画矩形，常用于标出人、车辆或报警区域。
 * @param ctx 程序传给通道处理函数的 ctx，直接传入即可。
 * @param rect 矩形的左上角 x/y 与宽/高，单位像素；宽和高应大于 0。
 * @param color 颜色按 cv::Scalar(蓝, 绿, 红) 填写，每项 0~255；例如 (0,0,255) 是红色。
 * @param thickness 线宽，单位像素；默认 2，填 -1 时画实心矩形。
 * @param alpha 颜色的不透明程度，填 0~1；0 看不见，0.3 可透出底图，1 完全不透明。
 * @param target 默认 ALL：实时画面、图片、视频都画；只画其中一项可选 DISPLAY、IMAGE、VIDEO。
 * @return 没有返回值；程序会把图形画到本次画面上，ctx 为空或当前不能绘制时忽略。
 * 位置按 640×640 图片填写，左上角是 (0,0)，向右 x 增大、向下 y 增大。
 * 示例：
 * @code
 * cv::Rect box(20, 20, 100, 80); // 左上角 (20,20)，宽 100、高 80
 * draw_rect(ctx, box, cv::Scalar(0, 0, 255), 2);
 * @endcode
 */
void draw_rect(ChannelContext *ctx, const cv::Rect &rect, const cv::Scalar &color = cv::Scalar(0, 255, 0),
               int thickness = 2, double alpha = 1.0, DrawCommand::Target target = DrawCommand::ALL);

/** @brief 在画面上画圆，适合标记一个位置或它附近的范围。
 * @param ctx 程序传给通道处理函数的 ctx，直接传入即可。
 * @param center 圆心的横、纵坐标。
 * @param radius 半径，单位像素，不能为负数。
 * @param color 颜色按 cv::Scalar(蓝, 绿, 红) 填写，每项 0~255；例如 (0,0,255) 是红色。
 * @param thickness 圆边线宽，默认 2 像素；填 -1 时画实心圆。
 * @param alpha 颜色的不透明程度，填 0~1；0 看不见，0.3 可透出底图，1 完全不透明。
 * @param target 默认 ALL：实时画面、图片、视频都画；只画其中一项可选 DISPLAY、IMAGE、VIDEO。
 * @return 没有返回值；程序会把图形画到本次画面上，ctx 为空或当前不能绘制时忽略。
 * 位置按 640×640 图片填写，左上角是 (0,0)，向右 x 增大、向下 y 增大。
 * 示例：
 * @code
 * cv::Point center = ctx->business_center();
 * draw_circle(ctx, center, 50); // 在画面中心画半径为 50 的绿色圆
 * @endcode
 */
void draw_circle(ChannelContext *ctx, const cv::Point &center, int radius,
                 const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2, double alpha = 1.0,
                 DrawCommand::Target target = DrawCommand::ALL);

/** @brief 在两个位置之间画一条直线，例如警戒线或两目标的连线。
 * @param ctx 程序传给通道处理函数的 ctx，直接传入即可。
 * @param pt1 线段起点的横、纵坐标。
 * @param pt2 线段终点的横、纵坐标。
 * @param color 颜色按 cv::Scalar(蓝, 绿, 红) 填写，每项 0~255；例如 (0,0,255) 是红色。
 * @param thickness 线宽，单位像素，应大于 0，默认 2。
 * @param target 默认 ALL：实时画面、图片、视频都画；只画其中一项可选 DISPLAY、IMAGE、VIDEO。
 * @return 没有返回值；程序会把图形画到本次画面上，ctx 为空或当前不能绘制时忽略。
 * 位置按 640×640 图片填写，左上角是 (0,0)，向右 x 增大、向下 y 增大。
 * 示例：
 * @code
 * cv::Point start(20, 200);
 * cv::Point end(600, 200);
 * draw_line(ctx, start, end, cv::Scalar(0, 0, 255), 2);
 * @endcode
 */
void draw_line(ChannelContext *ctx, const cv::Point &pt1, const cv::Point &pt2,
               const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2,
               DrawCommand::Target target = DrawCommand::ALL);

/** @brief 在画面上写文字，例如人数、状态或报警提示；支持中文。
 * @param ctx 程序传给通道处理函数的 ctx，直接传入即可。
 * @param text 要显示的文字；中文用 UTF-8 保存，std::string 变量传 .c_str()。
 * @param pos 文字左侧、靠近底部的位置，按 640×640 填写；例如 (20,40) 位于左上方。
 * @param color 颜色按 cv::Scalar(蓝, 绿, 红) 填写，每项 0~255；例如 (0,0,255) 是红色。
 * @param font_scale 字的大小比例，默认 0.6；填正数，数值越大字越大。
 * @param thickness 默认 1；填 2 或更大可加粗文字。
 * @param target 默认 ALL：实时画面、图片、视频都画；只画其中一项可选 DISPLAY、IMAGE、VIDEO。
 * @param shadow_enabled 是否给字加描边以便看清，默认 false；需要时填 true。
 * @param shadow_color 描边颜色，顺序同 color，默认黑色。
 * @param shadow_width 描边宽度，单位像素，至少为 1；只在开启描边时使用。
 * @return 没有返回值；程序会把文字画到本次画面上，空 ctx 或空 text 指针会被忽略。
 * 本函数会保存文字内容，调用后可以继续修改你自己的 message 变量。
 * 示例：
 * @code
 * std::string message = "人数：3";
 * draw_text(ctx, message.c_str(), {20, 40}, cv::Scalar(0, 0, 255));
 * @endcode
 */
void draw_text(ChannelContext *ctx, const char *text, const cv::Point &pos,
               const cv::Scalar &color = cv::Scalar(255, 255, 255), double font_scale = 0.6, int thickness = 1,
               DrawCommand::Target target = DrawCommand::ALL, bool shadow_enabled = false,
               const cv::Scalar &shadow_color = cv::Scalar(0, 0, 0), int shadow_width = 2);

/** @brief 给显示画面中指定的像素区域涂一层颜色，例如高亮分割结果。
 * @param ctx 程序传给通道处理函数的 ctx，直接传入即可。
 * @param mask 一张 CV_8UC1 黑白图；值为 0 的位置不涂色，非 0 的位置涂色。
 * @param color 颜色按 cv::Scalar(蓝, 绿, 红) 填写，每项 0~255；例如 (0,0,255) 是红色。
 * @param alpha 颜色所占比例，建议 0~1；0.3 为半透明高亮，达到 1 时完全覆盖。
 * @return true 表示处理完成；没有画面、图片格式错误或 alpha<=0 时返回 false。
 * mask 与显示图尺寸不同时会自动缩放；全黑的 mask 不改画面，但仍返回 true。
 * 示例：
 * @code
 * cv::Mat mask(640, 640, CV_8UC1, cv::Scalar(0));
 * mask(cv::Rect(100, 100, 200, 200)).setTo(255); // 选中这块矩形区域
 * bool painted = blend_display_mask(ctx, mask, cv::Scalar(0, 0, 255), 0.3);
 * @endcode
 */
bool blend_display_mask(ChannelContext *ctx, const cv::Mat &mask, const cv::Scalar &color, double alpha);

/** @brief 按顺序连接多个点，画轨迹、边界或区域轮廓。
 * @param ctx 程序传给通道处理函数的 ctx，直接传入即可。
 * @param points 按连接顺序排列的点，至少两个；每个点是横、纵坐标。
 * @param color 颜色按 cv::Scalar(蓝, 绿, 红) 填写，每项 0~255；例如 (0,0,255) 是红色。
 * @param thickness 线宽，单位像素，应大于 0，默认 2。
 * @param alpha 颜色的不透明程度，填 0~1；0 看不见，0.3 可透出底图，1 完全不透明。
 * @param closed true 会把最后一点连回第一点，画区域轮廓时通常选 true。
 * @param target 默认 ALL：实时画面、图片、视频都画；只画其中一项可选 DISPLAY、IMAGE、VIDEO。
 * @return 没有返回值；在本次画面上画线，少于两个点或当前不能绘制时忽略。
 * 位置按 640×640 图片填写，左上角是 (0,0)，向右 x 增大、向下 y 增大。
 * 示例：
 * @code
 * std::vector<cv::Point> points{{20, 20}, {100, 80}, {200, 20}};
 * draw_polyline(ctx, points, cv::Scalar(0, 255, 0), 2, 1.0, true);
 * @endcode
 */
void draw_polyline(ChannelContext *ctx, const std::vector<cv::Point> &points,
                   const cv::Scalar &color = cv::Scalar(0, 255, 0), int thickness = 2, double alpha = 1.0,
                   bool closed = false, DrawCommand::Target target = DrawCommand::ALL);

/** @brief 给多边形区域内部填色，适合突出网页上画出的关注区域。
 * @param ctx 程序传给通道处理函数的 ctx，直接传入即可。
 * @param points 按边界顺序排列的点，至少三个，位置按 640×640 图片填写。
 * @param color 颜色按 cv::Scalar(蓝, 绿, 红) 填写，每项 0~255；例如 (0,0,255) 是红色。
 * @param alpha 颜色的不透明程度，填 0~1；0 看不见，0.3 可透出底图，1 完全不透明。
 * @param target 默认 ALL：实时画面、图片、视频都画；只画其中一项可选 DISPLAY、IMAGE、VIDEO。
 * @return 没有返回值；默认半透明填色，少于三个点或当前不能绘制时忽略。
 * 示例：
 * @code
 * if (const auto *points = ctx->roi_polygon_at(0))
 * {
 *     draw_poly_filled(ctx, *points, cv::Scalar(0, 0, 255), 0.3);
 * }
 * @endcode
 */
void draw_poly_filled(ChannelContext *ctx, const std::vector<cv::Point> &points,
                      const cv::Scalar &color = cv::Scalar(0, 255, 0), double alpha = 0.3,
                      DrawCommand::Target target = DrawCommand::ALL);

/** @brief 检查当前能不能绘制中文等文字，方便在字体不可用时改用英文。
 * @return true 表示字体可用；false 表示无法加载字体。
 * 不需要传入参数。
 * 示例：
 * @code
 * const char *message = text_overlay_available() ? "人员" : "person";
 * draw_text(ctx, message, {20, 40});
 * @endcode
 */
bool text_overlay_available();

/** @brief 先算出一段文字要占多大地方，方便居中、对齐或画背景框。
 * @param utf8 要测量的文字，中文用 UTF-8。
 * @param font_height_px 期望字高，单位像素，例如 24；小于 1 时按 1 处理。
 * @param thickness 字的画法，-1 为填满字形，正数为笔画线宽；应与实际绘字时一致。
 * @param size 接收文字的宽、高；直接传 cv::Size 变量，成功后读 width/height。
 * @param baseline 接收文字定位线下方还要预留的像素数；字母 g 等可能延伸到这里。
 * @return true 表示测量成功，可以读取 size 和 baseline；空文字、缺少字体或测量失败返回 false。
 * 示例：
 * @code
 * cv::Size size;
 * int baseline = 0;
 * if (measure_text_unicode("人员", 24, -1, size, baseline))
 * {
 *     int text_width = size.width; // 可以用它计算文字居中时的横坐标
 * }
 * @endcode
 */
bool measure_text_unicode(const std::string &utf8, int font_height_px, int thickness, cv::Size &size, int &baseline);

/** @brief 直接在你提供的图片上写中文等文字，适合处理自己的图片。
 * @param img 要修改的非空图片，通常使用 CV_8UC3 彩色图片；绘制结果直接保存在它里面。
 * @param utf8 要显示的文字，中文用 UTF-8。
 * @param org 文字左侧、靠近底部的位置，按这张图片自身的像素坐标填写。
 * @param font_height_px 字高，单位像素，例如 24；小于 1 时按 1 处理。
 * @param color 颜色按 cv::Scalar(蓝, 绿, 红) 填写，每项 0~255；例如 (0,0,255) 是红色。
 * @param thickness 填 -1 画完整字形；正数指定字形轮廓的线宽。
 * @return true 表示已经写入图片；空图、空文字、字体不可用或绘制失败返回 false。
 * 本函数不会自动显示或保存图片；要显示它，可再调用 ctx->replace_display_frame(image)。
 * 示例：
 * @code
 * cv::Mat image(640, 640, CV_8UC3, cv::Scalar(0, 0, 0));
 * bool written = draw_text_unicode(image, "人员", {20, 40}, 24,
 *                                  cv::Scalar(255, 255, 255), -1);
 * @endcode
 */
bool draw_text_unicode(cv::InputOutputArray img, const std::string &utf8, cv::Point org, int font_height_px,
                       const cv::Scalar &color, int thickness);

namespace vision
{
/** @brief 给你自己的彩色图片中的指定区域涂色，处理结果直接留在这张图里。
 * @param bgr 要修改的非空 CV_8UC3 彩色图片，颜色通道顺序为蓝、绿、红。
 * @param mask CV_8UC1 黑白图：0 的位置不涂色，非 0 的位置涂色；尺寸不同时会自动缩放。
 * @param color 颜色按 cv::Scalar(蓝, 绿, 红) 填写，每项 0~255；例如 (0,0,255) 是红色。
 * @param alpha 颜色所占比例，建议 0~1；0.3 可透出原图，达到 1 时完全覆盖。
 * @return true 表示处理完成；图片格式错误、空图或 alpha<=0 时返回 false。全黑 mask 不改图但返回 true。
 * 示例：
 * @code
 * cv::Mat image(100, 100, CV_8UC3, cv::Scalar(0, 0, 0));
 * cv::Mat mask(100, 100, CV_8UC1, cv::Scalar(255)); // 全部位置都选中
 * bool painted = vision::blend_mask(image, mask, cv::Scalar(0, 0, 255), 0.3);
 * @endcode
 */
bool blend_mask(cv::Mat &bgr, const cv::Mat &mask, const cv::Scalar &color, double alpha);
} // namespace vision
