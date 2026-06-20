/**
 * @file logic_roll.cpp
 * @brief logic_roll —— 基于 HSV 的 ROI 物料占用率检测, 超阈值报警可视化。
 *        占用率算法见 logic_tools.h 的 logic_roll_compute_occupancy。
 */
#include "logic_common.h"

static void logic_roll(ChannelContext *ctx)
{
    if (!ctx)
        return;

    if (!ctx->roi || ctx->roi->size() < 3)
    {
        draw_text(ctx, "roll: roi invalid", cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.6, 2);
        return;
    }

    if (!ctx->frame || ctx->frame->empty())
    {
        draw_text(ctx, "roll: frame empty", cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.6, 2);
        return;
    }

    const int sat_min = 40, val_min = 40, hue_tol = 12, min_area = 100;
    const double threshold = 0.10;

    double ratio = 0.0;
    cv::Mat occupancy_mask;
    int bg_hue = 0;
    if (!logic_roll_compute_occupancy(*ctx->frame, *ctx->roi,
                                      sat_min, val_min, hue_tol, min_area,
                                      ratio, occupancy_mask, bg_hue))
    {
        draw_text(ctx, "roll: occupancy compute failed", cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.6, 2);
        return;
    }

    int alarm = (ratio > threshold);
    const cv::Scalar roi_color = alarm ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
    for (size_t i = 0; i < ctx->roi->size(); ++i)
    {
        const cv::Point &p1 = (*ctx->roi)[i];
        const cv::Point &p2 = (*ctx->roi)[(i + 1) % ctx->roi->size()];
        draw_line(ctx, p1, p2, roi_color, 2);
    }

    if (ctx->results)
    {
        for (auto &r : *ctx->results)
        {
            int in_roi = roi_contains(ctx, r.box, 0);   /* 是否落在测量区(第 0 个 ROI) */
            r.box_color = in_roi ? cv::Scalar(0, 255, 255) : cv::Scalar(120, 120, 120);
        }
    }

    if (!occupancy_mask.empty())
    {
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(occupancy_mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        for (const auto &c : contours)
        {
            if (cv::contourArea(c) < min_area)
                continue;
            cv::Rect box = cv::boundingRect(c);
            draw_rect(ctx, box, cv::Scalar(0, 0, 255), 2);
        }
    }

    char info1[180];
    snprintf(info1, sizeof(info1), "Roll occupancy: %.2f%% (th=%.2f%%) bg_hue=%d %s",
             ratio * 100.0, threshold * 100.0, bg_hue, alarm ? "ALARM" : "OK");
    draw_text(ctx, info1, cv::Point(20, 30), alarm ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0), 0.62, 2);

    char info2[180];
    snprintf(info2, sizeof(info2), "HSV rule: S>=%d V>=%d hue_tol=%d min_area=%d", sat_min, val_min, hue_tol, min_area);
    draw_text(ctx, info2, cv::Point(20, 56), cv::Scalar(255, 255, 255), 0.5, 1);
}

REGISTER_LOGIC("logic_roll", logic_roll);
