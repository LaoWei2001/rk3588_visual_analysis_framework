/**
 * @file logic_custom.cpp
 * @brief logic_custom —— ROI + 检测框 IN/OUT 统计与可视化示例(纯调试)。
 */
#include "logic_common.h"

static void logic_custom(ChannelContext *ctx)
{
    if (!ctx)
        return;

    int has_roi = (ctx->roi && ctx->roi->size() >= 3);

    if (has_roi)
    {
        for (size_t i = 0; i < ctx->roi->size(); ++i)
        {
            const cv::Point &p1 = (*ctx->roi)[i];
            const cv::Point &p2 = (*ctx->roi)[(i + 1) % ctx->roi->size()];
            draw_line(ctx, p1, p2, cv::Scalar(0, 255, 255), 2);
            draw_circle(ctx, p1, 3, cv::Scalar(0, 255, 255), 2);
        }
        cv::Point roi_txt((*ctx->roi).front().x + 4, std::max(20, (*ctx->roi).front().y - 8));
        draw_text(ctx, "ROI (scaled)", roi_txt, cv::Scalar(0, 255, 255), 0.55, 1);
    }
    else
    {
        draw_text(ctx, "ROI empty (pass-through)", cv::Point(20, 30), cv::Scalar(0, 165, 255), 0.6, 2);
    }

    int in_cnt = 0, out_cnt = 0;
    if (ctx->results)
    {
        for (auto &r : *ctx->results)
        {
            const cv::Point box_c = r.box_center();
            int in_roi = roi_contains(ctx, r.box, ROI_ALL);   /* 没画 ROI=整帧不设限(算在内) */

            if (in_roi)
                ++in_cnt;
            else
                ++out_cnt;

            r.box_color = in_roi ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
            draw_circle(ctx, box_c, 4, in_roi ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255), 2);

            char info[160];
            snprintf(info, sizeof(info), "%s %.2f %s", r.label.c_str(), r.score,
                     in_roi ? "IN_ROI" : "OUT_ROI");
            draw_text(ctx, info, cv::Point(r.box.x, std::max(20, r.box.y - 8)),
                      cv::Scalar(255, 255, 255), 0.5, 1);
        }
    }

    if (has_roi)
    {
        char summary[96];
        snprintf(summary, sizeof(summary), "ROI in:%d out:%d", in_cnt, out_cnt);
        draw_text(ctx, summary, cv::Point(20, 30), cv::Scalar(0, 255, 255), 0.6, 2);
    }

    char pipe_info[128];
    snprintf(pipe_info, sizeof(pipe_info), "Frame ID: %lld | dt: %.1fms | Infer: %s",
             (long long)ctx->frame_id, ctx->dt_ms, ctx->infer_enabled ? "ON" : "OFF");
    draw_text(ctx, pipe_info, cv::Point(20, 60), cv::Scalar(255, 128, 0), 0.6, 2);
}

REGISTER_LOGIC("logic_custom", logic_custom);
