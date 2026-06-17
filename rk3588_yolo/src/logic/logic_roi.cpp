/**
 * @file logic_roi.cpp
 * @brief logic_roi —— ROI 顶点坐标 + 检测框中心坐标可视化; 目标中心落在 ROI 内则染红。
 *        纯可视化/调试逻辑: 不报警、不上报、无可调参数、无跨帧状态。
 *        坐标系全部是模型输入尺寸(640), 与 ctx->roi / box 一致 —— 直接画即可。
 */
#include "logic_common.h"

static void logic_roi(ChannelContext *ctx)
{
    if (!ctx) return;

    const int has_roi = (ctx->roi && ctx->roi->size() >= 3);

    /* 1) ROI 各顶点：画边 + 顶点标记 + 在旁边标注该点坐标 */
    if (has_roi)
    {
        const size_t n = ctx->roi->size();
        for (size_t i = 0; i < n; ++i)
        {
            const cv::Point &p  = (*ctx->roi)[i];
            const cv::Point &pn = (*ctx->roi)[(i + 1) % n];
            draw_line(ctx, p, pn, cv::Scalar(0, 255, 255), 1);   /* ROI 边（黄、细） */
            draw_circle(ctx, p, 4, cv::Scalar(0, 255, 255), 2);  /* 顶点标记 */
            char vtxt[48];
            snprintf(vtxt, sizeof(vtxt), "(%d,%d)", p.x, p.y);
            draw_text(ctx, vtxt, cv::Point(p.x + 6, std::max(12, p.y - 6)),
                      cv::Scalar(0, 255, 255), 0.45, 1);         /* 顶点坐标（黄） */
        }
    }
    else
    {
        draw_text(ctx, "logic_roi: no ROI", cv::Point(20, 30),
                  cv::Scalar(0, 165, 255), 0.6, 2);
    }

    /* 2) 每个检测框：中心标注坐标；中心落在 ROI 内 → 该框染红 */
    if (ctx->results)
    {
        for (auto &r : *ctx->results)
        {
            const cv::Point c = r.box_center();
            const bool inside = roi_contains(ctx, r.box, ROI_ALL);   /* 没画 ROI=整帧不设限(算在内) */

            if (inside)
                r.box_color = cv::Scalar(0, 0, 255);             /* 红：中心在区域内 */

            const cv::Scalar mark = inside ? cv::Scalar(0, 0, 255)   /* 红 */
                                           : cv::Scalar(0, 255, 0);  /* 绿 */
            draw_circle(ctx, c, 3, mark, 2);                     /* 中心点标记 */
            char ctxt[48];
            snprintf(ctxt, sizeof(ctxt), "(%d,%d)", c.x, c.y);
            draw_text(ctx, ctxt, cv::Point(c.x + 6, c.y + 4), mark, 0.45, 1);
        }
    }
}

REGISTER_LOGIC("logic_roi", logic_roi);
