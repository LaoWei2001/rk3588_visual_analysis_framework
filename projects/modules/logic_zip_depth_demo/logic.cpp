#include <channel.h>
#include <cstdio>

static void logic_zip_depth_demo(ChannelContext *ctx)
{
    if (!ctx) return;
    const auto *depth = ctx->depth_result(ctx->param_string("depth_model_id"));
    const cv::Point center = ctx->business_center();
    const bool valid = depth && depth->is_relative_inverse_depth() && depth->depth_valid_at(center.x, center.y);
    const float value = valid ? depth->depth_at(center.x, center.y) : 0.0f;
    ctx->publish_bool("depth_valid", valid);
    ctx->publish_number("center_inverse_depth", value);
    if (depth && depth->is_relative_inverse_depth() && ctx->results)
        for (const auto &target : *ctx->results)
        {
            if (target.is_depth() || !depth->depth_valid_at(target.box_center().x, target.box_center().y)) continue;
            char text[80];
            std::snprintf(text, sizeof(text), "Relative: %.4f", depth->depth_median(target.box));
            draw_text(ctx, text, target.box_center(), cv::Scalar(0,255,255));
        }
    char text[100];
    if (valid) std::snprintf(text, sizeof(text), "Relative inverse depth: %.4f", value);
    else std::snprintf(text, sizeof(text), "ZipDepth unavailable");
    draw_circle(ctx, center, 5, cv::Scalar(0,255,255));
    draw_text(ctx, text, cv::Point(20,40));
}

REGISTER_LOGIC(logic_zip_depth_demo);
