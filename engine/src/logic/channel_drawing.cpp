#include "context_access.h"
#include <algorithm>
#include <channel.h>
#include <drawing.h>
#include <opencv2/imgproc.hpp>

/*======================== 绘制辅助函数实现 ========================*/
void draw_rect(ChannelContext *ctx, const cv::Rect &rect, const cv::Scalar &color, int thickness, double alpha,
               DrawCommand::Target target)
{
    if (!ctx || !VisionContextAccess::drawing(*ctx))
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::RECT;
    cmd.rect = rect;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.alpha = alpha;
    cmd.target = target;
    VisionContextAccess::drawing(*ctx)->push_back(cmd);
}

void draw_circle(ChannelContext *ctx, const cv::Point &center, int radius, const cv::Scalar &color, int thickness,
                 double alpha, DrawCommand::Target target)
{
    if (!ctx || !VisionContextAccess::drawing(*ctx))
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::CIRCLE;
    cmd.center = center;
    cmd.radius = radius;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.alpha = alpha;
    cmd.target = target;
    VisionContextAccess::drawing(*ctx)->push_back(cmd);
}

void draw_line(ChannelContext *ctx, const cv::Point &pt1, const cv::Point &pt2, const cv::Scalar &color, int thickness,
               DrawCommand::Target target)
{
    if (!ctx || !VisionContextAccess::drawing(*ctx))
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::LINE;
    cmd.pt1 = pt1;
    cmd.pt2 = pt2;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    VisionContextAccess::drawing(*ctx)->push_back(cmd);
}

void draw_text(ChannelContext *ctx, const char *text, const cv::Point &pos, const cv::Scalar &color, double font_scale,
               int thickness, DrawCommand::Target target, bool shadow_enabled, const cv::Scalar &shadow_color,
               int shadow_width)
{
    if (!ctx || !VisionContextAccess::drawing(*ctx) || !text)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::TEXT;
    cmd.text = text;
    cmd.text_pos = pos;
    cmd.font_scale = font_scale;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.text_shadow_enabled = shadow_enabled;
    cmd.text_shadow_color = shadow_color;
    cmd.text_shadow_width = std::max(1, shadow_width);
    cmd.target = target;
    VisionContextAccess::drawing(*ctx)->push_back(cmd);
}

bool blend_display_mask(ChannelContext *ctx, const cv::Mat &mask, const cv::Scalar &color, double alpha)
{
    if (!ctx || !VisionContextAccess::has_canvas(*ctx) || mask.empty() || mask.type() != CV_8UC1 || alpha <= 0.0)
        return false;
    return vision::blend_mask(ctx->display_canvas(), mask, color, alpha);
}

void draw_polyline(ChannelContext *ctx, const std::vector<cv::Point> &points, const cv::Scalar &color, int thickness,
                   double alpha, bool closed, DrawCommand::Target target)
{
    if (!ctx || !VisionContextAccess::drawing(*ctx) || points.size() < 2)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::POLYLINE;
    cmd.points = points;
    cmd.closed = closed;
    cmd.alpha = alpha;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    VisionContextAccess::drawing(*ctx)->push_back(cmd);
}

void draw_poly_filled(ChannelContext *ctx, const std::vector<cv::Point> &points, const cv::Scalar &color, double alpha,
                      DrawCommand::Target target)
{
    if (!ctx || !VisionContextAccess::drawing(*ctx) || points.size() < 3)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::POLY_FILLED;
    cmd.points = points;
    cmd.alpha = alpha;
    cmd.color = color;
    cmd.target = target;
    VisionContextAccess::drawing(*ctx)->push_back(cmd);
}
