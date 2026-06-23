/**
 * @file logic_cross_camera.cpp
 * @brief logic_cross_camera —— 跨通道取数演示: 在本通道画面上显示「另一通道」的
 *        同帧 frame/results/fps 信息(证明跨通道快照同帧一致)。
 */
#include "logic_common.h"

static void logic_cross_camera(ChannelContext *ctx)
{
    if (!ctx)
        return;

    int total_chns = app_ctrl_get_chn_nums();
    if (total_chns < 2)
    {
        draw_text(ctx, "Cross-Cam: Need >= 2 channels configured", cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.6, 2);
        return;
    }

    int target_chn = (ctx->chnId == 0) ? 1 : 0;
    if (target_chn >= total_chns)
        target_chn = 0;

    ChannelSnapshot snap;
    if (!app_ctrl_get_channel_snapshot(target_chn, &snap) || snap.frame.empty())
    {
        draw_text(ctx, "Cross-Cam: snapshot unavailable", cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.6, 2);
        return;
    }

    std::string logic_name = app_ctrl_get_logic_name(target_chn);

    int person_cnt = 0, has_car = 0;
    for (const auto &r : snap.results)
    {
        if (r.label == "person")
            ++person_cnt;
        if (r.label == "car")
            has_car = 1;
    }

    /* snap.frame 与 snap.results 同帧 (snap.frame_seq 为帧号), 这里把 seq/age
     * 一并显示, 直观证明跨通道拿到的"画面 + 框"来自同一时刻。 */
    char info1[128], info2[128], info3[128];
    snprintf(info1, sizeof(info1), "Target CH: %d | Logic: %s", target_chn, logic_name.c_str());
    snprintf(info2, sizeof(info2), "Target FPS: D=%.1f I=%.1f | seq=%lld age=%lldms", snap.disp_fps, snap.infer_fps,
             (long long)snap.frame_seq, (long long)snap.result_age_ms);
    snprintf(info3, sizeof(info3), "Target Obj: Person=%d Car=%s Total=%zu", person_cnt, has_car ? "YES" : "NO",
             snap.results.size());

    int base_y = 30;
    draw_text(ctx, "[Cross Camera Demo]", cv::Point(20, base_y), cv::Scalar(0, 255, 255), 0.6, 2);
    draw_text(ctx, info1, cv::Point(20, base_y + 30), cv::Scalar(255, 255, 255), 0.5, 1);
    draw_text(ctx, info2, cv::Point(20, base_y + 60), cv::Scalar(255, 255, 255), 0.5, 1);
    draw_text(ctx, info3, cv::Point(20, base_y + 90), cv::Scalar(255, 255, 255), 0.5, 1);
}

REGISTER_LOGIC("logic_cross_camera", logic_cross_camera);
