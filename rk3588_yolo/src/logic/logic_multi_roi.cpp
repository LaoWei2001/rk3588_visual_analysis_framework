/**
 * @file logic_multi_roi.cpp
 * @brief logic_multi_roi —— 演示"一个视频流配置多个 ROI 区域"的访问方式。
 *
 * 演示要点(都用 ChannelContext 的多 ROI 便捷方法, 见 channel_logic.h):
 *   ctx->roi_count()                       本通道 ROI 区域数量
 *   ctx->roi_name_at(i) / roi_polygon_at(i)  按序号取区域名 / 多边形
 *   ctx->roi_by_name("entrance")           按名字取区域
 *   ctx->target_count_in_roi("person", i)  统计第 i 个区域内的 person 数量
 *   ctx->target_count_in_roi_named(...)    按名字统计
 *
 * 行为: 逐区域用不同颜色画出多边形 + 标注[序号]名字和该区域 person 数;
 *       每个检测框按"落在哪个区域"染成该区域颜色(不在任何区域=灰);
 *       顶部显示区域总数, 若存在名为 "entrance" 的区域则单独显示其人数。
 * 纯可视化, 不报警不上报, 无可调参数, 无跨帧状态。
 * 坐标系全部是模型输入尺寸(640), 与 ctx->rois / box 一致, 直接画。
 */
#include "logic_common.h"

static void logic_multi_roi(ChannelContext *ctx)
{
    if (!ctx) return;

    /* 每个区域一种颜色(BGR), 区域多于色板时循环复用 */
    static const cv::Scalar kPalette[] = {
        cv::Scalar(0, 255, 255),  /* 黄   */
        cv::Scalar(0, 255, 0),    /* 绿   */
        cv::Scalar(255, 128, 0),  /* 蓝青 */
        cv::Scalar(255, 0, 255),  /* 品红 */
        cv::Scalar(0, 165, 255),  /* 橙   */
        cv::Scalar(255, 255, 0),  /* 青   */
    };
    const int kNPal = (int)(sizeof(kPalette) / sizeof(kPalette[0]));

    const int nroi = ctx->roi_count();
    if (nroi == 0)
    {
        draw_text(ctx, "logic_multi_roi: no ROI (draw zones in web console)",
                  cv::Point(20, 30), cv::Scalar(0, 165, 255), 0.6, 2);
        return;
    }

    /* 1) 逐区域: 画闭合多边形(各自颜色) + 在首个顶点旁标注 [序号] 名字 person=N */
    for (int i = 0; i < nroi; ++i)
    {
        const std::vector<cv::Point> *poly = ctx->roi_polygon_at(i);
        if (!poly || poly->size() < 3) continue;
        const cv::Scalar col = kPalette[i % kNPal];

        draw_polyline(ctx, *poly, col, 2, 1.0, /*closed=*/true);

        const char *nm      = ctx->roi_name_at(i);
        const int   persons = ctx->target_count_in_roi("person", i);
        char label[96];
        if (nm && nm[0])
            snprintf(label, sizeof(label), "[%d] %s  person=%d", i, nm, persons);
        else
            snprintf(label, sizeof(label), "[%d] zone  person=%d", i, persons);
        const cv::Point anchor(poly->front().x + 4, std::max(16, poly->front().y - 6));
        draw_text(ctx, label, anchor, col, 0.5, 1);
    }

    /* 2) 逐检测框: 找它中心落在哪个区域 → 染成该区域颜色; 不在任何区域 → 灰 */
    if (ctx->results)
    {
        for (auto &r : *ctx->results)
        {
            const cv::Point c = r.box_center();
            int hit = -1;
            for (int i = 0; i < nroi; ++i)
            {
                const std::vector<cv::Point> *poly = ctx->roi_polygon_at(i);
                if (poly && poly->size() >= 3 &&
                    cv::pointPolygonTest(*poly, c, false) >= 0.0)
                {
                    hit = i;
                    break;
                }
            }
            const cv::Scalar col = (hit >= 0) ? kPalette[hit % kNPal]
                                              : cv::Scalar(160, 160, 160);
            r.box_color = col;
            draw_circle(ctx, c, 3, col, 2);
        }
    }

    /* 3) 顶部汇总 + 演示"按名字取区域"(ctx->roi_by_name / target_count_in_roi_named) */
    char summary[64];
    snprintf(summary, sizeof(summary), "ROI zones: %d", nroi);
    draw_text(ctx, summary, cv::Point(20, 24), cv::Scalar(255, 255, 255), 0.6, 2);

    if (ctx->roi_by_name("entrance"))
    {
        const int n = ctx->target_count_in_roi_named("person", "entrance");
        char t[64];
        snprintf(t, sizeof(t), "entrance person=%d", n);
        draw_text(ctx, t, cv::Point(20, 48), cv::Scalar(0, 255, 0), 0.55, 2);
    }
}

REGISTER_LOGIC("logic_multi_roi", logic_multi_roi);
