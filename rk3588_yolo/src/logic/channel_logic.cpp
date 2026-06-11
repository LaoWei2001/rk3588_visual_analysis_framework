/**
 * @file channel_logic.cpp
 * @brief 通道自定义业务逻辑 — C-style
 *
 * 架构说明:
 * - 跟踪器 (Tracker) 已移至 analyzer.cpp 全局执行
 * - 此文件仅用于用户自定义业务扩展
 *
 * 添加自定义逻辑步骤:
 *   1. 在本文件实现 static void logic_xxx(ChannelContext* ctx) 函数
 *   2. 在 channel_logic_init() 中调用 register_logic("logic_xxx", logic_xxx) 注册
 *   3. 在 config.json 中将对应通道的 "logic" 字段设为 "logic_xxx"
 */

#include "../uploader/alarm_uploader.h"
#include <opencv2/opencv.hpp>
#include "channel_logic.h"
#include "../analyzer/algoProcess.h"
#include "../core/app_ctrl.h"
#include "../player/text_overlay.h"   /* draw_text_unicode: 画面/上报图文字统一走 freetype(中英文) */
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#include "logic_tools.h"

/*======================== ChannelContext 跨通道方法实现 ========================*/
ChannelSnapshot ChannelContext::get_channel_snapshot(int chnId) const
{
    ChannelSnapshot out;
    app_ctrl_get_channel_snapshot(chnId, &out);
    return out;
}

std::string ChannelContext::get_channel_logic_name(int chnId) const
{
    return app_ctrl_get_logic_name(chnId);
}

int ChannelContext::channel_has_logic(int chnId, const char *logicName) const
{
    return app_ctrl_get_logic_name(chnId) == std::string(logicName) ? 1 : 0;
}

/*======================== 绘制辅助函数实现 ========================*/
void draw_rect(ChannelContext *ctx, const cv::Rect &rect,
               const cv::Scalar &color, int thickness,
               DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::RECT;
    cmd.rect = rect;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

void draw_circle(ChannelContext *ctx, const cv::Point &center, int radius,
                 const cv::Scalar &color, int thickness,
                 DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::CIRCLE;
    cmd.center = center;
    cmd.radius = radius;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

void draw_line(ChannelContext *ctx, const cv::Point &pt1, const cv::Point &pt2,
               const cv::Scalar &color, int thickness,
               DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::LINE;
    cmd.pt1 = pt1;
    cmd.pt2 = pt2;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

void draw_text(ChannelContext *ctx, const char *text, const cv::Point &pos,
               const cv::Scalar &color, double font_scale, int thickness,
               DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds || !text)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::TEXT;
    cmd.text = text;
    cmd.text_pos = pos;
    cmd.font_scale = font_scale;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

void draw_polyline(ChannelContext *ctx, const std::vector<cv::Point> &points,
                   const cv::Scalar &color, int thickness,
                   double alpha, bool closed,
                   DrawCommand::Target target)
{
    if (!ctx || !ctx->draw_cmds || points.size() < 2)
        return;
    DrawCommand cmd;
    cmd.type = DrawCommand::POLYLINE;
    cmd.points = points;
    cmd.closed = closed;
    cmd.alpha = alpha;
    cmd.color = color;
    cmd.thickness = thickness;
    cmd.target = target;
    ctx->draw_cmds->push_back(cmd);
}

/*======================== 逻辑实现 ========================*/

static void logic_default(ChannelContext *ctx)
{
    (void)ctx;
}

struct ServerState
{
    uint64_t last_upload_ms = 0;
};

static void logic_server(ChannelContext *ctx)
{
    if (!ctx || !ctx->results || ctx->results->empty())
        return;
    if (!ctx->frame)
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<ServerState>();
    auto &s = *std::static_pointer_cast<ServerState>(*ctx->state);

    /* 上报间隔(秒)→ms：每帧从 ctx->config 现读以支持热重载；首次(last=0)立即上报，之后按间隔冷却 */
    const uint64_t interval_ms =
        (uint64_t)std::max(1, ctx->config ? ctx->config->report_interval_sec : 5) * 1000ULL;
    if (ctx->timestamp_ms - s.last_upload_ms < interval_ms)
        return;

    alarm_uploader_enqueue(*ctx->frame, *ctx->frame, ctx->chnId, "intrusion",
                           ctx->config ? ctx->config->server_url.c_str() : nullptr);
    s.last_upload_ms = ctx->timestamp_ms;
}

struct DifyState
{
    uint64_t last_upload_ms = 0;
    int first = 1;
};

static void logic_dify(ChannelContext *ctx)
{
    if (!ctx || !ctx->results || ctx->results->empty())
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<DifyState>();
    auto &s = *std::static_pointer_cast<DifyState>(*ctx->state);

    /* 上报间隔(秒)→ms：每帧从 ctx->config 现读以支持热重载 */
    const uint64_t interval_ms =
        (uint64_t)std::max(1, ctx->config ? ctx->config->report_interval_sec : 5) * 1000ULL;

    if (s.first)
    {
        s.last_upload_ms = ctx->timestamp_ms;
        s.first = 0;
        return;
    }

    if (ctx->timestamp_ms - s.last_upload_ms < interval_ms)
        return;
    if (!ctx->frame || ctx->frame->empty())
        return;

    const char *prompt = (ctx->config && !ctx->config->dify_prompt.empty())
                             ? ctx->config->dify_prompt.c_str()
                             : "无提示词";

    char event_id[64];
    snprintf(event_id, sizeof(event_id), "ch%d_f%lld_t%llu",
             ctx->chnId, (long long)ctx->frame_id, (unsigned long long)ctx->timestamp_ms);

    const char *dify_url = (ctx->config && !ctx->config->dify_api_url.empty()) ? ctx->config->dify_api_url.c_str() : nullptr;
    const char *dify_key = (ctx->config && !ctx->config->dify_api_key.empty()) ? ctx->config->dify_api_key.c_str() : nullptr;
    dify_uploader_enqueue(*ctx->frame, prompt, event_id, dify_url, dify_key);

    s.last_upload_ms = ctx->timestamp_ms;
}

static void logic_hook(ChannelContext *ctx)
{
    if (!ctx)
        return;
    if (!*ctx->state)
        *ctx->state = std::make_shared<HookState>();
    auto &s = *std::static_pointer_cast<HookState>(*ctx->state);

    const cv::Point safe_center(330, 320);
    const int radius = ctx->config ? ctx->config->radius : 1;
    const int radius_sq = radius * radius;
    const float alarm_duration_sec = 0.8f;
    const float reset_duration_sec = 3.0f;
    const float dt_sec = ctx->dt_ms / 1000.0f;
    const uint64_t no_detect_grace_ms = 500;

    int found_hook = 0;
    int hook_outside = 0;
    if (ctx->results)
    {
        for (AlgoResult &r : *ctx->results)
        {
            if ((r.class_id != 0) || (r.label != "hook_l"))
                continue;
            found_hook = 1;
            int is_outside = (r.dist_sq_to(safe_center) > radius_sq);
            hook_outside = hook_outside || is_outside;
        }
    }

    if (found_hook)
        s.last_found_ms = ctx->timestamp_ms;

    int in_grace = !found_hook && (ctx->timestamp_ms - s.last_found_ms <= no_detect_grace_ms);

    const cv::Scalar circle_color = (hook_outside || (in_grace && s.alarm_active))
                                        ? cv::Scalar(0, 0, 255)
                                        : cv::Scalar(0, 255, 0);
    draw_circle(ctx, safe_center, radius, circle_color, 2, DrawCommand::ALL);

    /* 状态面板 */
    {
        const int tx = 410, ty = 330, line_h = 45;
        char line1[64], line2[64];
        if (s.alarm_sent_latch)
            snprintf(line1, sizeof(line1), "OUT: %.1fs [ALARM]", s.disp_outside_sec);
        else
            snprintf(line1, sizeof(line1), "OUT: %.1fs", s.disp_outside_sec);
        draw_text(ctx, line1, cv::Point(tx, ty), cv::Scalar(0, 0, 255), 1, 2, DrawCommand::DISPLAY);

        if (s.alarm_sent_latch)
            snprintf(line2, sizeof(line2), "IN:  %.1fs / %.0fs", s.disp_inside_sec, reset_duration_sec);
        else
            snprintf(line2, sizeof(line2), "IN:  %.1fs", s.disp_inside_sec);
        draw_text(ctx, line2, cv::Point(tx, ty + line_h), cv::Scalar(0, 255, 0), 1, 2, DrawCommand::DISPLAY);

        const char *status_str;
        cv::Scalar status_color;
        if (in_grace)
        {
            status_str = "SEARCHING";
            status_color = cv::Scalar(0, 200, 255);
        }
        else if (!found_hook)
        {
            status_str = "NO HOOK";
            status_color = cv::Scalar(180, 180, 180);
        }
        else if (!s.alarm_sent_latch)
        {
            status_str = "READY";
            status_color = cv::Scalar(0, 255, 0);
        }
        else if (s.alarm_reset_counting)
        {
            status_str = "COOLING";
            status_color = cv::Scalar(0, 165, 255);
        }
        else
        {
            status_str = "ALARMED";
            status_color = cv::Scalar(0, 0, 255);
        }
        draw_text(ctx, status_str, cv::Point(tx, ty + line_h * 2), status_color, 1, 2, DrawCommand::DISPLAY);
    }

    if (in_grace)
        return;

    if (hook_outside)
    {
        s.alarm_reset_counting = false;
        s.disp_inside_sec = 0.0f;

        if (!s.alarm_active)
        {
            s.alarm_active = true;
            s.alarm_start_ms = ctx->timestamp_ms;
        }

        if (!s.alarm_sent_latch)
            s.disp_outside_sec += dt_sec;

        float outside_elapsed = (ctx->timestamp_ms - s.alarm_start_ms) / 1000.0f;
        if (!s.alarm_sent_latch && outside_elapsed >= alarm_duration_sec)
        {
            if (ctx->frame)
            {
                cv::Mat upload_img = ctx->frame->clone();
                RenderParams rp = ctx->render_params();
                rp.show_fps = 0;
                rp.target_mask = DrawCommand::UPLOAD;
                render_overlays(upload_img, rp);
                alarm_uploader_enqueue(upload_img, *ctx->frame, ctx->chnId, "hook_tilt",
                                       ctx->config ? ctx->config->server_url.c_str() : nullptr);
            }
            s.alarm_sent_latch = true;
        }
    }
    else
    {
        s.alarm_active = false;
        if (!s.alarm_sent_latch)
            s.disp_outside_sec = 0.0f;

        if (s.alarm_sent_latch && found_hook)
        {
            s.disp_inside_sec += dt_sec;

            if (!s.alarm_reset_counting)
            {
                s.alarm_reset_counting = true;
                s.alarm_reset_start_ms = ctx->timestamp_ms;
            }
            else
            {
                float reset_elapsed = (ctx->timestamp_ms - s.alarm_reset_start_ms) / 1000.0f;
                if (reset_elapsed >= reset_duration_sec)
                {
                    s.alarm_sent_latch = false;
                    s.alarm_reset_counting = false;
                    s.disp_outside_sec = 0.0f;
                    s.disp_inside_sec = 0.0f;
                }
            }
        }
        else
        {
            s.alarm_reset_counting = false;
            s.disp_inside_sec = 0.0f;
        }
    }
}

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
            int in_roi = (cv::pointPolygonTest(*ctx->roi, r.box_center(), false) >= 0.0);
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
            int in_roi = !has_roi || (cv::pointPolygonTest(*ctx->roi, box_c, false) >= 0.0);

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

static void logic_person_alarm(ChannelContext *ctx)
{
    if (!ctx)
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<PersonAlarmState>();
    auto &s = *std::static_pointer_cast<PersonAlarmState>(*ctx->state);

    s.person_detected = false;
    s.person_count = 0;

    int has_roi = (ctx->roi && ctx->roi->size() >= 3);
    if (ctx->results)
    {
        for (auto &r : *ctx->results)
        {
            if (r.label != "person")
                continue;
            if (has_roi && cv::pointPolygonTest(*ctx->roi, r.box_center(), false) < 0)
                continue;

            s.person_detected = true;
            s.person_count++;

            r.box_color = cv::Scalar(0, 0, 255);
            char label[64];
            snprintf(label, sizeof(label), "person %.2f", r.score);
            draw_text(ctx, label, cv::Point(r.box.x, std::max(20, r.box.y - 8)),
                      cv::Scalar(0, 0, 255), 0.5, 1);
        }
    }

    if (s.person_detected)
    {
        char alarm_text[128];
        snprintf(alarm_text, sizeof(alarm_text), "ALARM: %d person(s) detected", s.person_count);
        draw_text(ctx, alarm_text, cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.7, 2);
    }
    else
    {
        draw_text(ctx, "CLEAR", cv::Point(20, 30), cv::Scalar(0, 255, 0), 0.7, 2);
    }

    if (has_roi)
    {
        for (size_t i = 0; i < ctx->roi->size(); ++i)
        {
            const cv::Point &p1 = (*ctx->roi)[i];
            const cv::Point &p2 = (*ctx->roi)[(i + 1) % ctx->roi->size()];
            draw_line(ctx, p1, p2, s.person_detected ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0), 2);
        }
    }
}

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

    /* snap.frame 与 snap.results 同帧 (snap.frame_seq 为帧号), 这里把 seq/age 一并显示,
     * 直观证明跨通道拿到的"画面 + 框"来自同一时刻。 */
    char info1[128], info2[128], info3[128];
    snprintf(info1, sizeof(info1), "Target CH: %d | Logic: %s", target_chn, logic_name.c_str());
    snprintf(info2, sizeof(info2), "Target FPS: D=%.1f I=%.1f | seq=%lld age=%lldms",
             snap.disp_fps, snap.infer_fps, (long long)snap.frame_seq, (long long)snap.result_age_ms);
    snprintf(info3, sizeof(info3), "Target Obj: Person=%d Car=%s Total=%zu",
             person_cnt, has_car ? "YES" : "NO", snap.results.size());

    int base_y = 30;
    draw_text(ctx, "[Cross Camera Demo]", cv::Point(20, base_y), cv::Scalar(0, 255, 255), 0.6, 2);
    draw_text(ctx, info1, cv::Point(20, base_y + 30), cv::Scalar(255, 255, 255), 0.5, 1);
    draw_text(ctx, info2, cv::Point(20, base_y + 60), cv::Scalar(255, 255, 255), 0.5, 1);
    draw_text(ctx, info3, cv::Point(20, base_y + 90), cv::Scalar(255, 255, 255), 0.5, 1);
}

/**
 * =====================================================
 * 用户自定义逻辑扩展区
 * =====================================================
 */

/*======================== logic_dify_person_verify — 人员检测 Dify 二次核验 ========================*/

/**
 * 功能: 当画面中出现未上报过的人员时, 上报至 Dify 进行二次核验。
 * 利用 tracker 分配的 track_id 防止对同一人员的重复报警。
 *
 * 去重策略:
 *   - 每帧收集当前画面中所有 person 的 track_id (需 track_id >= 0).
 *   - 新出现的 track_id 触发 Dify 上报; 已在 reported_ids 中的跳过.
 *   - track_id 连续丢失 MISS_FRAME_MAX 帧后才从 reported_ids 清除 (容忍短暂遮挡/漏检).
 *   - 两次上报之间至少间隔 MIN_INTERVAL_MS.
 */
static void logic_dify_person_verify(ChannelContext *ctx)
{
    if (!ctx || !ctx->results || ctx->results->empty())
        return;
    if (!ctx->frame || ctx->frame->empty())
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<DifyPersonVerifyState>();
    auto &s = *std::static_pointer_cast<DifyPersonVerifyState>(*ctx->state);

    static constexpr uint64_t MIN_INTERVAL_MS = 5000;
    static constexpr int MISS_FRAME_MAX = 30;         // 连续丢帧超过此数才清除 track_id
    static constexpr uint64_t DBG_INTERVAL_MS = 2000; // 调试打印间隔

    if (s.first)
    {
        s.last_upload_ms = ctx->timestamp_ms;
        s.last_dbg_ms = ctx->timestamp_ms;
        s.first = 0;
        printf("[DifyPersonVerify] ch=%d init, ts=%llu\n", ctx->chnId, (unsigned long long)ctx->timestamp_ms);
        return;
    }

    int has_roi = (ctx->roi && ctx->roi->size() >= 3);

    /* 收集当前帧所有有效的人员 track_id */
    std::set<int> current_person_ids;
    int total_person = 0, no_track = 0, out_roi = 0;
    for (auto &r : *ctx->results)
    {
        if (r.label != "person")
            continue;
        total_person++;
        if (r.track_id < 0)
        {
            no_track++;
            continue;
        }
        if (has_roi && cv::pointPolygonTest(*ctx->roi, r.box_center(), false) < 0)
        {
            out_roi++;
            continue;
        }

        current_person_ids.insert(r.track_id);
        r.box_color = cv::Scalar(0, 0, 255);
    }

    /* ---- 更新 miss_frames ---- */
    /* 仍在画面中的: 清零 */
    for (int tid : current_person_ids)
        s.miss_frames[tid] = 0;

    /* 不在画面中但在 reported_ids / miss_frames 中的: 递增 */
    {
        std::vector<int> expired;
        for (auto &kv : s.miss_frames)
        {
            int tid = kv.first;
            if (current_person_ids.find(tid) != current_person_ids.end())
                continue; // 上面已清零
            kv.second++;
            if (kv.second > MISS_FRAME_MAX)
                expired.push_back(tid);
        }
        for (int tid : expired)
        {
            s.miss_frames.erase(tid);
            s.reported_ids.erase(tid);
        }
    }

    /* ---- 找出新出现的人员 (不在 reported_ids 中) ---- */
    bool has_new_person = false;
    int new_tid = -1;
    for (int tid : current_person_ids)
    {
        if (s.reported_ids.find(tid) == s.reported_ids.end())
        {
            has_new_person = true;
            new_tid = tid;
            break;
        }
    }

    /* ---- 上报至 Dify ---- */
    if (has_new_person && (ctx->timestamp_ms - s.last_upload_ms >= MIN_INTERVAL_MS))
    {
        const char *prompt = (ctx->config && !ctx->config->dify_prompt.empty())
                                 ? ctx->config->dify_prompt.c_str()
                                 : "person detected, please verify";

        char event_id[128];
        snprintf(event_id, sizeof(event_id), "ch%d_f%lld_t%llu_person",
                 ctx->chnId, (long long)ctx->frame_id, (unsigned long long)ctx->timestamp_ms);

        /* 克隆帧并在上面绘制所有目标框 + id/类别/置信度 */
        cv::Mat upload_img = ctx->frame->clone();
        int img_w = upload_img.cols;
        int img_h = upload_img.rows;

        for (auto &r : *ctx->results)
        {
            /* 将检测框 clamp 到图像边界内 */
            int bx = std::max(0, r.box.x);
            int by = std::max(0, r.box.y);
            int bw = std::min(r.box.width, img_w - bx);
            int bh = std::min(r.box.height, img_h - by);
            if (bw <= 0 || bh <= 0)
                continue;

            cv::Rect safe_box(bx, by, bw, bh);

            cv::Scalar color;
            int thickness;
            if (r.label == "person")
            {
                color = cv::Scalar(0, 0, 255);
                thickness = 2;
            }
            else
            {
                color = cv::Scalar(0, 255, 0);
                thickness = 1;
            }

            cv::rectangle(upload_img, safe_box, color, thickness);

            char label_text[96];
            if (r.track_id >= 0)
                snprintf(label_text, sizeof(label_text), "id:%d %s %.2f", r.track_id, r.label.c_str(), r.score);
            else
                snprintf(label_text, sizeof(label_text), "%s %.2f", r.label.c_str(), r.score);

            int baseline = 0;
            cv::Size text_sz = cv::getTextSize(label_text, cv::FONT_HERSHEY_SIMPLEX, 0.45, 1, &baseline);
            int txt_w = text_sz.width + 4;
            int txt_h = text_sz.height + 4;

            /* 文字放在框上方，若上方空间不够则放在框内顶部 */
            int txt_x = std::max(0, bx);
            int txt_y = by - 6;
            if (txt_y - txt_h < 0)
                txt_y = by + txt_h + 2;

            /* clamp 文字 ROI 到图像边界 */
            txt_x = std::min(txt_x, img_w - txt_w);
            txt_y = std::min(std::max(txt_y, txt_h), img_h);

            cv::Rect roi_rect(txt_x, txt_y - txt_h, txt_w, txt_h);
            cv::Mat roi = upload_img(roi_rect);
            cv::Mat overlay;
            roi.copyTo(overlay);
            cv::rectangle(overlay, cv::Rect(0, 0, roi.cols, roi.rows), color, -1);
            cv::addWeighted(overlay, 0.5, roi, 0.5, 0, roi);

            draw_text_unicode(upload_img, label_text,
                              cv::Point(txt_x + 2, txt_y - 4),
                              /*px*/ 14, cv::Scalar(255, 255, 255), /*filled*/ -1);
        }

        printf("[DifyPersonVerify] ch=%d UPLOAD: new_tid=%d frame=%lld ts=%llu\n",
               ctx->chnId, new_tid, (long long)ctx->frame_id, (unsigned long long)ctx->timestamp_ms);
        const char *dify_url = (ctx->config && !ctx->config->dify_api_url.empty()) ? ctx->config->dify_api_url.c_str() : nullptr;
        const char *dify_key = (ctx->config && !ctx->config->dify_api_key.empty()) ? ctx->config->dify_api_key.c_str() : nullptr;
        dify_uploader_enqueue(upload_img, prompt, event_id, dify_url, dify_key);
        s.last_upload_ms = ctx->timestamp_ms;

        /* 把当前所有可见 track_id 都标记为已上报, 防止同一帧多人轮流触发 */
        for (int tid : current_person_ids)
        {
            s.reported_ids.insert(tid);
            s.miss_frames[tid] = 0;
        }
    }

    /* ---- 定期调试打印 ---- */
    if (ctx->timestamp_ms - s.last_dbg_ms >= DBG_INTERVAL_MS)
    {
        printf("[DifyPersonVerify] ch=%d frame=%lld | total_person=%d no_track=%d out_roi=%d | "
               "tracked=%zu reported=%zu miss=%zu | last_up=%llu\n",
               ctx->chnId, (long long)ctx->frame_id,
               total_person, no_track, out_roi,
               current_person_ids.size(), s.reported_ids.size(), s.miss_frames.size(),
               (unsigned long long)s.last_upload_ms);
        s.last_dbg_ms = ctx->timestamp_ms;
    }

    /* ---- 状态绘制 ---- */
    char status[128];
    snprintf(status, sizeof(status), "DifyVerify: %zu person(s) | rep:%zu",
             current_person_ids.size(), s.reported_ids.size());
    draw_text(ctx, status, cv::Point(20, 30),
              current_person_ids.empty() ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255),
              0.6, 2);

    /* 为每个检测到的人员绘制 track_id */
    for (auto &r : *ctx->results)
    {
        if (r.label != "person" || r.track_id < 0)
            continue;
        if (has_roi && cv::pointPolygonTest(*ctx->roi, r.box_center(), false) < 0)
            continue;

        char tid_text[32];
        snprintf(tid_text, sizeof(tid_text), "id:%d", r.track_id);
        draw_text(ctx, tid_text,
                  cv::Point(r.box.x, std::max(20, r.box.y - 8)),
                  cv::Scalar(0, 255, 255), 0.45, 1);
    }
}

/*======================== 逻辑分发表 ========================*/
static LogicEntry g_logic_registry[MAX_LOGIC_FUNCS];
static int g_logic_count = 0;

/* logic_roi —— ROI 顶点坐标 + 检测框中心坐标可视化；目标中心落在 ROI 内则该框染红。
 * 纯可视化/调试逻辑：不报警、不上报、无可调参数、无跨帧状态。
 * 坐标系全部是模型输入尺寸(640)，与 ctx->roi / box 一致——直接画即可。 */
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
            const bool inside = has_roi &&
                                cv::pointPolygonTest(*ctx->roi, c, false) >= 0.0;

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

/* logic_multi_roi —— 演示"一个视频流配置多个 ROI 区域"的访问方式。
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
 * 坐标系全部是模型输入尺寸(640), 与 ctx->rois / box 一致, 直接画。 */
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

/* ============================================================================
 * logic_wafer_sop —— 晶圆湿法清洗 SOP 合规检测 (装晶圆的特氟龙花篮转移清洗)
 *
 * 全程"逐工序点亮"可视化完成度(不做结算): 左上一列清单, 每行一开始红色, 达成即变绿。
 * 默认 6 行(4 槽 + 抖动 + 朝向):
 *   1..N  各槽(sop_sequence): 花篮"真正进入"(持续 enter_ms 停留确认)该槽 → 该行变绿; 漏掉的槽保持红。
 *   N+1   抖动次数: 进入最后一个槽(纯水槽)后才计数, 跟踪框中心 Y 上下往复, 每达 sop_shake_amplitude
 *         记一次; 次数 >= sop_shake_min_count → 变绿。
 *   N+2   朝向: 默认绿"方向未变化"; 类别(默认 bkt_normal/bkt_abnormal)持续 sop_dir_sec 翻转后
 *         → 红"方向变化"。
 * 另: 高亮花篮当前实际所在槽(粗框); 花篮框按已确认朝向上色; 进入首槽=开新一轮(清空清单),
 *     花篮长时间离场也会自动复位。
 *
 * 依赖: 本通道按 sop_sequence 各名字画好 ROI 区域(ctx->rois)。纯画面提示(不上报)。
 * 坐标系: 模型输入尺寸(640)。画面文字经 freetype 渲染, 直接显示中文(槽名/抖动次数/方向)。
 * 可调参数见 logics.json。
 * ==========================================================================*/

/* 逗号分隔字符串 → 去首尾空白的非空片段 */
static std::vector<std::string> sop_split_csv(const std::string &s)
{
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        size_t b = cur.find_first_not_of(" \t\r\n");
        size_t e = cur.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
        cur.clear();
    };
    for (char c : s) { if (c == ',') flush(); else cur += c; }
    flush();
    return out;
}

struct WaferSopState
{
    int              cur_zone = -1;         /* 最近"确认进入"的 seq 槽下标(去重用) */
    std::vector<int> visited;               /* 已"确认进入"(点亮)的 seq 槽下标集合 */

    /* 进入区域防抖: 候选槽须持续 enter_ms 才确认进入 */
    int              cand_zone     = -2;    /* 当前帧所在槽(瞬时); -1=转移, -2=无花篮 */
    uint64_t         cand_since_ms = 0;     /* 候选槽起始时刻 */

    /* 朝向切换防抖: 新类别须持续 dir_ms 才确认 */
    int      dir_cand         = -1;         /* 瞬时类别: 0=正常 1=翻转 -1=无花篮 */
    uint64_t dir_cand_since_ms = 0;
    int      dir_confirmed    = 0;          /* 已确认朝向: 0=正常 1=翻转 */
    bool     reversed_seen    = false;      /* 本轮工序内确认出现过翻转 */

    /* 抖动(纯水槽=最后一个槽内) */
    bool  in_shake    = false;
    int   shake_count = 0;
    float ext_y       = -1.0f;              /* 当前方向上的极值 Y */
    int   y_dir       = 0;                  /* +1 向下 / -1 向上 / 0 未定 */

    uint64_t last_seen_ms = 0;              /* 最近一次检测到花篮(任意位置); 长时间不见 → 复位本轮 */
};

static void logic_wafer_sop(ChannelContext *ctx)
{
    if (!ctx) return;
    if (!*ctx->state) *ctx->state = std::make_shared<WaferSopState>();
    auto &s = *std::static_pointer_cast<WaferSopState>(*ctx->state);

    /* ---- 参数(每帧现读, 支持网页热重载) ---- */
    const ChannelConfig *cfg = ctx->config;
    const std::vector<std::string> seq = sop_split_csv(cfg ? cfg->sop_sequence : std::string());
    const std::string normLab = cfg ? cfg->basket_normal_label   : std::string("bkt_normal");
    const std::string abnLab  = cfg ? cfg->basket_abnormal_label : std::string("bkt_abnormal");
    const int shakeAmp = cfg ? cfg->sop_shake_amplitude : 15;
    const int shakeMin = cfg ? cfg->sop_shake_min_count : 3;
    const float enterSec = cfg ? cfg->sop_enter_sec : 0.8f;   /* 进入区域确认时长 */
    const float dirSec   = cfg ? cfg->sop_dir_sec   : 0.5f;   /* 朝向切换确认时长 */
    const uint64_t enter_ms = (uint64_t)std::max(0, (int)(enterSec * 1000.0f + 0.5f));
    const uint64_t dir_ms   = (uint64_t)std::max(0, (int)(dirSec   * 1000.0f + 0.5f));
    const int nseq    = (int)seq.size();
    const int lastIdx = nseq - 1;             /* 最后一个槽 = 纯水槽 = 抖动槽 */
    const int      MOVE_EPS = 2;              /* Y 抖动死区(px) */
    const uint64_t RESET_MS = 6000;           /* 花篮离场超此时长 → 清空本轮清单(非结算, 仅复位) */

    if (nseq < 2)
    {
        draw_text(ctx, "wafer_sop: set 'sop_sequence' + draw named ROI zones",
                  cv::Point(20, 30), cv::Scalar(0, 165, 255), 0.55, 2);
        return;
    }

    /* ---- 找"花篮"(置信度最高的 normal/abnormal 检测) ---- */
    AlgoResult *basket = nullptr;
    bool basket_reversed = false;
    if (ctx->results)
    {
        float best = -1.0f;
        for (auto &r : *ctx->results)
        {
            const bool isNorm = (r.label == normLab);
            const bool isAbn  = (r.label == abnLab);
            if (!isNorm && !isAbn) continue;
            if (r.score > best) { best = r.score; basket = &r; basket_reversed = isAbn; }
        }
    }

    /* ---- 花篮中心落在哪个 seq 槽(命名区域) ---- */
    cv::Point center(-1, -1);
    int zoneNow = -1;
    if (basket)
    {
        center = basket->box_center();
        for (int i = 0; i < nseq; ++i)
        {
            const RoiZone *z = ctx->roi_by_name(seq[i].c_str());
            if (z && z->polygon.size() >= 3 &&
                cv::pointPolygonTest(z->polygon, center, false) >= 0.0)
            { zoneNow = i; break; }
        }
    }

    const uint64_t now = ctx->timestamp_ms;

    /* 花篮长时间离场 → 清空本轮清单(无结算, 仅为下一篮复位) */
    if (basket) s.last_seen_ms = now;
    else if (s.last_seen_ms != 0 && (now - s.last_seen_ms) > RESET_MS) s = WaferSopState();

    /* ---- ② 朝向(类别)防抖: 新类别须持续 dir_ms 才确认翻转/恢复, 滤除瞬时误检 ---- */
    if (basket)
    {
        const int cls = basket_reversed ? 1 : 0;
        if (cls != s.dir_cand) { s.dir_cand = cls; s.dir_cand_since_ms = now; }
        if (s.dir_cand != s.dir_confirmed && (now - s.dir_cand_since_ms) >= dir_ms)
        {
            s.dir_confirmed = s.dir_cand;
            if (s.dir_confirmed == 1) s.reversed_seen = true;
        }
    }
    else s.dir_cand = -1;                          /* 花篮消失: 重置, 重新出现需重新计时 */
    const bool revConfirmed = (s.dir_confirmed == 1);
    const bool revPending   = basket && (s.dir_cand != s.dir_confirmed);

    /* ---- ① 进入区域防抖 + 逐槽点亮(完成度) ----
     * 花篮中心须在同一 seq 槽内持续 enter_ms 才算"真正进入"(滤除转移时短暂划过);
     * 一旦确认进入某槽就把它点亮(visited)。不做结算: 完成度全程实时反映在左上清单。
     * 进入首槽(去胶槽1)视为开新一轮 → 先清空上一篮的清单。*/
    if (!basket)                       s.cand_zone = -2;
    else if (zoneNow != s.cand_zone) { s.cand_zone = zoneNow; s.cand_since_ms = now; }

    if (basket && s.cand_zone >= 0 && s.cand_zone != s.cur_zone &&
        (now - s.cand_since_ms) >= enter_ms)
    {
        const int z = s.cand_zone;
        if (z == 0)                                   /* 进入首槽 = 开新一轮, 清空清单 */
        {
            const uint64_t seen = s.last_seen_ms;
            s = WaferSopState();
            s.last_seen_ms = seen;
            s.cand_zone = 0; s.cand_since_ms = now;
        }
        if (std::find(s.visited.begin(), s.visited.end(), z) == s.visited.end())
            s.visited.push_back(z);                   /* 点亮该槽 */
        s.cur_zone = z;

        if (z == lastIdx && !s.in_shake)              /* 确认进入纯水槽 → 开始抖动计数 */
        { s.in_shake = true; s.shake_count = 0; s.ext_y = (float)center.y; s.y_dir = 0; }
        if (z != lastIdx) s.in_shake = false;
    }

    /* ---- ③ 抖动检测(纯水槽内, 跟踪框中心 Y 的上下往复) ---- */
    if (s.in_shake && basket && zoneNow == lastIdx)
    {
        const float y = (float)center.y;
        if (s.y_dir == 0)
        {
            if (y - s.ext_y >= MOVE_EPS) { s.y_dir = +1; s.ext_y = y; }
            else if (s.ext_y - y >= MOVE_EPS) { s.y_dir = -1; s.ext_y = y; }
        }
        else if (s.y_dir > 0)                      /* 正在向下(Y 增大) */
        {
            if (y > s.ext_y) s.ext_y = y;          /* 继续探底 */
            else if (s.ext_y - y >= (float)shakeAmp) { s.shake_count++; s.y_dir = -1; s.ext_y = y; }
        }
        else                                       /* 正在向上(Y 减小) */
        {
            if (y < s.ext_y) s.ext_y = y;
            else if (y - s.ext_y >= (float)shakeAmp) { s.shake_count++; s.y_dir = +1; s.ext_y = y; }
        }
    }

    /* 某 seq 槽是否已"确认进入"(点亮) */
    auto visited_has = [&](int i) {
        return std::find(s.visited.begin(), s.visited.end(), i) != s.visited.end();
    };

    /* ===================== 画面提示(全 ASCII) ===================== */
    /* 1) 各 seq 槽多边形: 已点亮=绿 / 未到=黄; 花篮当前实际所在槽=粗框。首顶点旁标 ASCII tag */
    for (int i = 0; i < nseq; ++i)
    {
        const RoiZone *z = ctx->roi_by_name(seq[i].c_str());
        if (!z || z->polygon.size() < 3) continue;
        const bool isCur = (i == zoneNow);
        const cv::Scalar col = visited_has(i) ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 255, 255);
        draw_polyline(ctx, z->polygon, col, isCur ? 4 : 2, 1.0, true);
        const cv::Point a(z->polygon.front().x + 4, std::max(14, z->polygon.front().y - 6));
        draw_text(ctx, seq[i].c_str(), a, col, 0.5, 1);   /* 直接用真实槽名(中文) */
    }

    /* 2) 花篮框/中心点: 按"已确认朝向"上色(正常绿/翻转红) */
    if (basket)
    {
        const cv::Scalar bcol = revConfirmed ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
        basket->box_color = bcol;
        draw_circle(ctx, center, 4, bcol, 2);
    }

    /* 3) 左上"工序完成度"清单: 每行 红=未达成 / 绿=已达成, 逐工序点亮(全程实时, 无结算) */
    const cv::Scalar GREEN(0, 238, 0), RED(0, 0, 230);   /* 深绿 / 深红, 不刺眼 */
    int  yrow = 28;
    char line[96];
    /* 3.1 各槽: "真正进入"即点亮; 漏掉/未到的保持红 */
    for (int i = 0; i < nseq; ++i)
    {
        snprintf(line, sizeof(line), "%d. %s", i + 1, seq[i].c_str());
        draw_text(ctx, line, cv::Point(20, yrow), visited_has(i) ? GREEN : RED, 0.6, 2);
        yrow += 28;
    }
    /* 3.2 抖动次数(进入纯水槽后才计数): 达到阈值点亮 */
    snprintf(line, sizeof(line), "%d. 抖动次数 %d/%d", nseq + 1, s.shake_count, shakeMin);
    draw_text(ctx, line, cv::Point(20, yrow), (s.shake_count >= shakeMin) ? GREEN : RED, 0.6, 2);
    yrow += 28;
    /* 3.3 朝向: 未变化=绿 / 变化=红 */
    snprintf(line, sizeof(line), "%d. %s%s", nseq + 2,
             s.reversed_seen ? "方向变化" : "方向未变化",
             revPending ? "(确认中)" : "");
    draw_text(ctx, line, cv::Point(20, yrow), s.reversed_seen ? RED : GREEN, 0.6, 2);
}

/* logic_wafer —— 晶圆盘擦拭工序管理(轨迹/覆盖率/动作识别/计时/合规报警)
 *
 * 背景：AI 视觉管理"人工用工具擦拭晶圆盘"工序。模型类别：0=cleanwiper(工具尖端), 1=wafer(晶圆盘)。
 * 计算区域 = wafer(class 1) 检测框本身(动态)，不用手画 ROI。
 * 流程(状态机)：
 *   - 工序开始：cleanwiper 中心进入 wafer 框并持续 >= t_start 秒 → 开始(计时/记轨迹/算覆盖)。
 *     确认期滤除手短暂进入、反光/闪光误检；期间不画不计。
 *   - 进行中：每帧把工具中心连成轨迹，按 line_width 拓宽成"扫过面积带"(单色半透明铺底，
 *     不再实时分类/染色)；左上实时显示 覆盖率(以实时 wafer 框为基准)/用时/状态。
 *   - 工序结束：工具离开区域或消失持续 >= t_end 秒 → 结算：① 覆盖率(以"结束那一刻 wafer 框"
 *     为基准) >= coverage_threshold；② 把整条轨迹交给 wafer_detect_missing(点集, required) 离线
 *     分析有无漏动作(横/纵/弧)。两者都过 → 合格；否则右上角红字报警(驻留到下一工序)。
 * 不上报；坐标全部在模型输入坐标系(640)，与检测 box 一致。可调参数见 logics.json。 */
struct WaferState
{
    std::vector<cv::Point> trajectory;  /* cleanwiper 在区域内的轨迹点(模型坐标, 顺序即编号) */

    cv::Mat   swept_mask;               /* 扫过掩码(CV_8UC1)，每帧由轨迹重建 */
    cv::Size  mask_size;                /* swept_mask 对应帧尺寸 */
    cv::Rect  region_box;               /* 计算区域 = wafer(class 1)检测框(最近一次) */
    bool      have_region    = false;

    /* 开始(start)/结束(end) 计时 */
    uint64_t  enter_ms       = 0;       /* 工序开始确认窗口起点(0=未在确认) */
    uint64_t  region_last_ms = 0;       /* 最近一次在区域内的时刻(开始/结束判定共用) */
    bool      op_active      = false;   /* 是否已确认并在进行一次工序 */
    uint64_t  op_start_ms    = 0;       /* 工序开始时刻(确认通过) */

    /* 上次工序结算结果(驻留显示) */
    bool        have_result   = false;
    bool        last_pass     = true;
    float       last_coverage = 0.0f;   /* % */
    float       last_op_sec   = 0.0f;
    std::string last_msg;               /* 不合规原因(多行用 '\n' 分隔, ASCII) */
};

/* 动作英文短名(屏幕用 Hershey 字体不支持中文，故用 ASCII)；1横/2纵/3弧 */
static const char *wafer_action_name(int a)
{
    switch (a) { case 1: return "H"; case 2: return "V"; case 3: return "ARC"; default: return "?"; }
}

/* ===== 动作识别(仅结算时离线分析整条轨迹，不再实时分类/染色) ===== */

/* 一个窗口的运动学特征 */
struct WaferMotionFeat
{
    bool   valid       = false;
    double extent      = 0.0;   /* 主轴方向标准差(像素) */
    double linearity   = 0.0;   /* 1 - λ2/λ1 (1=共线/直线, →0=各向同性) */
    double trav_dx     = 0.0;   /* 窗口内 X 方向逐步行程 Σ|dx|(判横/纵, 抗整体漂移) */
    double trav_dy     = 0.0;   /* 窗口内 Y 方向逐步行程 Σ|dy| */
    double circ_resid  = 1e9;   /* 圆拟合归一化残差 rms|dist-R|/R(越小越像圆) */
    double circ_R      = 0.0;   /* 拟合半径(像素) */
    double arc_path    = 0.0;   /* 沿圆累计角程(弧度) */
    int    reversals   = 0;     /* 主轴投影方向反转次数(往复=擦) */
    double periodicity = 0.0;   /* 频域主频(非DC)能量占比 0~1(越大越周期/往复) */
};

/* 对一段连续点 p[0..m-1] 算特征：PCA(线性) + 逐步行程(横/纵) + Kåsa 圆拟合(残差/角程)
 * + 主轴投影反转计数 与 DFT 主频能量占比(频域周期性)。 */
static WaferMotionFeat wafer_motion_features(const cv::Point *p, int m)
{
    WaferMotionFeat f;
    if (!p || m < 6) return f;

    /* 均值 + 中心化二阶矩 */
    double mx = 0.0, my = 0.0;
    for (int i = 0; i < m; ++i) { mx += p[i].x; my += p[i].y; }
    mx /= m; my /= m;
    double Suu = 0, Svv = 0, Suv = 0, Suz = 0, Svz = 0, Sz = 0;
    for (int i = 0; i < m; ++i)
    {
        double u = p[i].x - mx, v = p[i].y - my, z = u * u + v * v;
        Suu += u * u; Svv += v * v; Suv += u * v;
        Suz += u * z; Svz += v * z; Sz += z;
    }

    /* PCA：特征值 + 主轴方向 */
    double tr = Suu + Svv, det2 = Suu * Svv - Suv * Suv;
    double disc = std::sqrt(std::max(0.0, tr * tr / 4.0 - det2));
    double l1 = tr / 2.0 + disc, l2 = tr / 2.0 - disc;
    if (l1 < 1e-9) return f;
    f.extent    = std::sqrt(l1 / m);
    f.linearity = 1.0 - (l2 / l1);
    double th = 0.5 * std::atan2(2.0 * Suv, Suu - Svv);

    /* 逐步行程(判横/纵)：捕捉来回往复的方向，抗"擦时整体漂移"，比位置散布更准 */
    double trav_dx = 0.0, trav_dy = 0.0;
    for (int i = 1; i < m; ++i)
    {
        trav_dx += std::fabs(static_cast<double>(p[i].x - p[i - 1].x));
        trav_dy += std::fabs(static_cast<double>(p[i].y - p[i - 1].y));
    }
    f.trav_dx = trav_dx; f.trav_dy = trav_dy;

    /* Kåsa 圆拟合(中心化坐标)：解 2x2 得圆心(uc,vc)、半径 R、归一化残差、角程 */
    double cdet = Suu * Svv - Suv * Suv;
    if (std::fabs(cdet) > 1e-6)
    {
        double uc = (Svv * (0.5 * Suz) - Suv * (0.5 * Svz)) / cdet;
        double vc = (Suu * (0.5 * Svz) - Suv * (0.5 * Suz)) / cdet;
        double R2 = uc * uc + vc * vc + Sz / m;
        if (R2 > 1e-6)
        {
            double R = std::sqrt(R2); f.circ_R = R;
            double se = 0.0, prevphi = 0.0, arc = 0.0; bool hp = false;
            for (int i = 0; i < m; ++i)
            {
                double u = p[i].x - mx, v = p[i].y - my;
                double du = u - uc, dv = v - vc, dist = std::sqrt(du * du + dv * dv);
                se += (dist - R) * (dist - R);
                double phi = std::atan2(dv, du);
                if (hp) { double d = phi - prevphi; while (d > CV_PI) d -= 2 * CV_PI; while (d < -CV_PI) d += 2 * CV_PI; arc += std::fabs(d); }
                prevphi = phi; hp = true;
            }
            f.circ_resid = std::sqrt(se / m) / R;
            f.arc_path   = arc;
        }
    }

    /* 主轴投影 → 反转次数(带死区) + 频域主频能量占比(DFT) */
    double ux = std::cos(th), uy = std::sin(th);
    std::vector<float> proj(m);
    for (int i = 0; i < m; ++i) proj[i] = static_cast<float>((p[i].x - mx) * ux + (p[i].y - my) * uy);

    double dead = 0.15 * f.extent + 1.0;
    int rev = 0; double lastdir = 0.0;
    for (int i = 1; i < m; ++i)
    {
        double dd = proj[i] - proj[i - 1];
        if (std::fabs(dd) < dead) continue;
        double dir = (dd > 0) ? 1.0 : -1.0;
        if (lastdir != 0.0 && dir != lastdir) ++rev;
        lastdir = dir;
    }
    f.reversals = rev;

    {
        double pm = 0.0; for (float x : proj) pm += x; pm /= m;
        std::vector<float> sig(m);
        for (int i = 0; i < m; ++i)
        {
            double w = 0.5 - 0.5 * std::cos(2.0 * CV_PI * i / (m - 1));   /* Hann 窗 */
            sig[i] = static_cast<float>((proj[i] - pm) * w);
        }
        cv::Mat spec;
        cv::dft(cv::Mat(sig), spec, cv::DFT_COMPLEX_OUTPUT);
        double tot = 0.0, best = 0.0;
        for (int k = 1; k <= m / 2; ++k)
        {
            cv::Vec2f c = spec.at<cv::Vec2f>(k);
            double e = static_cast<double>(c[0]) * c[0] + static_cast<double>(c[1]) * c[1];
            tot += e; if (e > best) best = e;
        }
        f.periodicity = (tot > 1e-9) ? best / tot : 0.0;
    }

    f.valid = true;
    return f;
}

/* 一个窗口判一个标签：横擦(1)/纵擦(2)/圆弧擦(3)/未知(0)。 */
static int wafer_window_label(const cv::Point *p, int m)
{
    WaferMotionFeat f = wafer_motion_features(p, m);
    if (!f.valid || f.extent < 3.0) return 0;
    /* 圆弧：圆拟合残差小 + 角程足够 + 非共线 + 半径合理 */
    if (f.circ_resid < 0.12 && f.arc_path >= 1.0 && f.linearity < 0.93 && f.circ_R > 4.0) return 3;
    /* 直线往复门控(确认是"擦"而非噪声漂移) */
    bool wipe = (f.linearity > 0.6) || (f.reversals >= 1) || (f.periodicity > 0.35);
    if (!wipe) return 0;
    /* 横/纵：逐步行程 Σ|dx| vs Σ|dy|(抗整体漂移) */
    if (f.trav_dx >= f.trav_dy * 1.15) return 1;
    if (f.trav_dy >= f.trav_dx * 1.15) return 2;
    return 0;
}

/* 轨迹点(坐标 + 编号)——结算分析函数的输入元素 */
struct WaferTrajPoint { cv::Point pt; int id; };

/* 解析 required_actions(逗号/空格/斜杠/分号分隔；支持 横擦/纵擦/圆弧擦 或 h/v/arc)
 * → 需校验的动作 id 列表(1=横,2=纵,3=弧)；空串 → 空列表(不校验)。 */
static std::vector<int> wafer_parse_required(const std::string &req)
{
    bool need_h = false, need_v = false, need_arc = false;
    std::string tok;
    auto flush = [&](std::string t)
    {
        size_t a = t.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return;
        size_t b = t.find_last_not_of(" \t\r\n");
        t = t.substr(a, b - a + 1);
        if (t.find("\xE6\xA8\xAA") != std::string::npos || t == "h" || t == "H") need_h = true;          /* 横 */
        else if (t.find("\xE7\xBA\xB5") != std::string::npos || t == "v" || t == "V") need_v = true;     /* 纵 */
        else if (t.find("\xE5\xBC\xA7") != std::string::npos || t.find("\xE5\x9C\x86") != std::string::npos
                 || t == "arc" || t == "ARC" || t == "a" || t == "A") need_arc = true;                   /* 弧/圆 */
    };
    for (char ch : req)
    {
        if (ch == ',' || ch == ' ' || ch == '/' || ch == ';' || ch == '\t' || ch == '\n')
        { flush(tok); tok.clear(); }
        else tok.push_back(ch);
    }
    flush(tok);

    std::vector<int> out;
    if (need_h)   out.push_back(1);
    if (need_v)   out.push_back(2);
    if (need_arc) out.push_back(3);
    return out;
}

/* 【结算时离线判漏动作】——按需求设计的核心函数。
 *   输入1 pts      : 有序轨迹点集合(每个含坐标 pt + 编号 id)；
 *   输入2 required : 需校验的动作 id 列表(0/1/多个；空=不校验)；
 *   返回           : required 中"没出现过"的动作 id 列表(缺失项)。
 * 方法：对整条轨迹滑动窗口逐段分类(圆拟合判弧 / 逐步行程判横纵)，统计各动作出现的
 *      窗口数，达到支持度阈值即认定该动作出现过，最后求缺失。 */
static std::vector<int> wafer_detect_missing(const std::vector<WaferTrajPoint> &pts,
                                             const std::vector<int> &required)
{
    if (required.empty()) return {};

    std::vector<cv::Point> xy; xy.reserve(pts.size());     /* 按编号取有序坐标(轨迹本就有序) */
    for (const auto &q : pts) xy.push_back(q.pt);
    const int n = static_cast<int>(xy.size());

    const int WIN = 28, STEP = 4;
    int cnt[4] = {0, 0, 0, 0};
    int nwin = 0;
    for (int e = WIN; e <= n; e += STEP)
    {
        int lab = wafer_window_label(xy.data() + (e - WIN), WIN);
        if (lab >= 1 && lab <= 3) ++cnt[lab];
        ++nwin;
    }
    if (nwin == 0 && n >= 6)            /* 轨迹太短不足一窗：整体判一次 */
    {
        int lab = wafer_window_label(xy.data(), n);
        if (lab >= 1 && lab <= 3) ++cnt[lab];
        nwin = 1;
    }

    /* 支持度：>=2 个窗口、或 >=12% 的窗口判为该动作，才算"出现过" */
    const int need = std::max(2, static_cast<int>(std::lround(0.12 * nwin)));
    std::vector<int> miss;
    for (int a : required)
        if (a >= 1 && a <= 3 && cnt[a] < need) miss.push_back(a);
    return miss;
}

static void logic_wafer(ChannelContext *ctx)
{
    if (!ctx || !ctx->results) return;
    if (!ctx->frame || ctx->frame->empty())
    {
        draw_text(ctx, "logic_wafer: frame empty", cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.6, 2);
        return;
    }

    if (!*ctx->state) *ctx->state = std::make_shared<WaferState>();
    auto &s = *std::static_pointer_cast<WaferState>(*ctx->state);

    const cv::Size frame_size = ctx->frame->size();
    if (s.swept_mask.empty() || s.mask_size != frame_size)
    {
        s.swept_mask = cv::Mat::zeros(frame_size, CV_8UC1);
        s.mask_size  = frame_size;
    }

    /* ---- 可调参数(每帧现读，热重载) ---- */
    int line_width = ctx->config ? ctx->config->line_width : 20;
    if (line_width < 1) line_width = 1;
    float t_start = ctx->config ? ctx->config->t_start : 1.0f;
    if (t_start < 0.0f) t_start = 0.0f;
    float t_end = ctx->config ? ctx->config->t_end : 1.0f;
    if (t_end < 0.1f) t_end = 0.1f;
    float cov_thresh = ctx->config ? ctx->config->coverage_threshold : 80.0f;
    const std::string req_actions = ctx->config ? ctx->config->required_actions : std::string();
    const uint64_t t_start_ms = static_cast<uint64_t>(t_start * 1000.0f);
    const uint64_t t_end_ms   = static_cast<uint64_t>(t_end   * 1000.0f);
    const uint64_t now = ctx->timestamp_ms;

    /* ---- 计算区域 = wafer(class 1) 最高分框(缓存最近一次) ---- */
    float best_region_score = -1.0f;
    for (auto &r : *ctx->results)
    {
        if (r.class_id != 1) continue;
        if (r.score > best_region_score) { best_region_score = r.score; s.region_box = r.box; s.have_region = true; }
    }
    cv::Rect region;
    if (s.have_region)
        region = s.region_box & cv::Rect(0, 0, frame_size.width, frame_size.height);
    if (region.area() <= 0)
    {
        draw_text(ctx, "logic_wafer: waiting for wafer (region)", cv::Point(20, 30), cv::Scalar(0, 165, 255), 0.6, 2);
        return;
    }

    /* ---- cleanwiper(class 0)：是否在画面 / 区域内最高分点 ---- */
    bool found_in_frame = false, have_new_pt = false;
    cv::Point new_pt; float best_score = -1.0f;
    for (auto &r : *ctx->results)
    {
        if (r.class_id != 0) continue;          /* 0=cleanwiper(按下标，比类名稳) */
        found_in_frame = true;
        r.box_color = cv::Scalar(0, 255, 0);
        const cv::Point c = r.box_center();
        if (region.contains(c) && r.score > best_score) { best_score = r.score; new_pt = c; have_new_pt = true; }
    }
    if (have_new_pt) s.region_last_ms = now;

    /* ---- 工序开始(T_start)：cleanwiper 在区域内持续 t_start 才正式开始 ---- */
    const uint64_t region_grace_ms = 400;       /* 确认期内区域短暂丢检的容忍 */
    if (!s.op_active)
    {
        if (have_new_pt) { if (s.enter_ms == 0) s.enter_ms = now; }
        else if (now - s.region_last_ms > region_grace_ms) s.enter_ms = 0;

        if (have_new_pt && s.enter_ms != 0 && (now - s.enter_ms >= t_start_ms))
        {
            s.op_active   = true;
            s.op_start_ms = now;
            s.trajectory.clear();
            s.have_result = false;              /* 新工序 → 撤掉上次告警 */
        }
    }

    /* ---- 记录新点(仅工序进行中；不实时分类，结算时统一离线分析) ---- */
    if (s.op_active && have_new_pt)
    {
        bool add = s.trajectory.empty();
        if (!add)
        {
            const cv::Point d = new_pt - s.trajectory.back();
            if (d.x * d.x + d.y * d.y >= 9) add = true;     /* 位移 >=3px 才记 */
        }
        if (add) s.trajectory.push_back(new_pt);
    }

    /* ---- 扫过掩码 + 实时覆盖率(以实时 wafer 框为基准) ---- */
    double ratio = 0.0;
    s.swept_mask.setTo(0);
    if (s.trajectory.size() == 1)
        cv::circle(s.swept_mask, s.trajectory[0], line_width / 2, cv::Scalar(255), -1);
    else if (s.trajectory.size() >= 2)
        cv::polylines(s.swept_mask, s.trajectory, false, cv::Scalar(255), line_width, cv::LINE_8);
    if (!s.trajectory.empty())
    {
        const double region_area = region.area();
        if (region_area > 0.0) ratio = cv::countNonZero(s.swept_mask(region)) / region_area;
    }

    /* ---- 工序结束(T_end)：cleanwiper 离开区域/消失持续 t_end → 结算 ---- */
    float end_elapsed = 0.0f;
    if (s.op_active)
    {
        end_elapsed = (now - s.region_last_ms) / 1000.0f;
        if (now - s.region_last_ms >= t_end_ms)
        {
            /* 覆盖率以"结束此刻 wafer 框"为基准(即当前 region，已在 ratio 中) */
            s.last_coverage = static_cast<float>(ratio * 100.0);
            s.last_op_sec   = (s.region_last_ms - s.op_start_ms) / 1000.0f;

            bool pass = true; std::string msg;
            if (s.last_coverage + 1e-3f < cov_thresh)
            {
                pass = false;
                char b[64]; snprintf(b, sizeof(b), "Coverage %.0f%% < %.0f%%", s.last_coverage, cov_thresh);
                msg = b;
            }
            /* 离线判漏动作：构造(坐标+编号)点集 → 调判定函数 wafer_detect_missing */
            std::vector<int> required = wafer_parse_required(req_actions);
            if (!required.empty())
            {
                std::vector<WaferTrajPoint> pts; pts.reserve(s.trajectory.size());
                for (int i = 0; i < static_cast<int>(s.trajectory.size()); ++i)
                    pts.push_back({s.trajectory[i], i});
                std::vector<int> miss = wafer_detect_missing(pts, required);
                if (!miss.empty())
                {
                    pass = false;
                    std::string ms;
                    for (size_t i = 0; i < miss.size(); ++i) { if (i) ms += ","; ms += wafer_action_name(miss[i]); }
                    if (!msg.empty()) msg += "\n";
                    msg += "Missing: " + ms;
                }
            }
            s.last_pass   = pass;
            s.last_msg    = msg;
            s.have_result = true;

            s.op_active = false;          /* 复位，等待下一工序 */
            s.trajectory.clear();
            s.enter_ms = 0;
        }
    }

    /* ---- 绘制：单色半透明轨迹(底层) → 区域框 → 文字 ---- */
    const cv::Scalar trail_color(255, 0, 255);   /* 品红，单色(不再按动作染色) */
    if (s.trajectory.size() == 1)
        draw_circle(ctx, s.trajectory[0], line_width / 2, trail_color, -1);
    else if (s.trajectory.size() >= 2)
        draw_polyline(ctx, s.trajectory, trail_color, line_width, 0.5);

    draw_rect(ctx, s.region_box, cv::Scalar(0, 255, 255), 2);   /* wafer 框=测量区域(黄) */

    /* 左上角：覆盖率 / 状态+动作 / 用时 */
    const float disp_cov = s.op_active ? static_cast<float>(ratio * 100.0)
                                       : (s.have_result ? s.last_coverage : 0.0f);
    char l1[80];
    snprintf(l1, sizeof(l1), "Coverage: %.1f%% / %.0f%%", disp_cov, cov_thresh);
    draw_text(ctx, l1, cv::Point(20, 30),
              (disp_cov + 1e-3f >= cov_thresh) ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 200, 255), 0.7, 2);

    char l2[120]; cv::Scalar l2c;
    if (s.op_active && have_new_pt)
    { snprintf(l2, sizeof(l2), "TRACKING pts:%zu", s.trajectory.size()); l2c = cv::Scalar(0, 255, 0); }
    else if (s.op_active)
    { snprintf(l2, sizeof(l2), "ENDING %.1f/%.1fs", end_elapsed, t_end); l2c = cv::Scalar(0, 200, 255); }
    else if (s.enter_ms != 0)
    { float dw = (now - s.enter_ms) / 1000.0f; snprintf(l2, sizeof(l2), "CONFIRMING %.1f/%.1fs", dw, t_start); l2c = cv::Scalar(0, 165, 255); }
    else if (found_in_frame)
    { snprintf(l2, sizeof(l2), "wiper out of region"); l2c = cv::Scalar(180, 180, 180); }
    else
    { snprintf(l2, sizeof(l2), "idle"); l2c = cv::Scalar(180, 180, 180); }
    draw_text(ctx, l2, cv::Point(20, 60), l2c, 0.6, 2);

    char l3[64];
    if (s.op_active)        { float run = (now - s.op_start_ms) / 1000.0f; snprintf(l3, sizeof(l3), "Time: %.1fs", run); }
    else if (s.have_result)  snprintf(l3, sizeof(l3), "Last: %.1fs", s.last_op_sec);
    else                     snprintf(l3, sizeof(l3), "Time: 0.0s");
    draw_text(ctx, l3, cv::Point(20, 90), cv::Scalar(0, 255, 255), 0.7, 2);

    /* 右上角：结算结果(不合规红字驻留, 合格绿字; 直到下一工序开始) */
    if (s.have_result && !s.op_active)
    {
        std::vector<std::string> lines;
        if (s.last_pass) lines.push_back("PASS");
        else
        {
            lines.push_back("NG");
            size_t p0 = 0;
            while (p0 <= s.last_msg.size())
            {
                size_t nl = s.last_msg.find('\n', p0);
                std::string ln = s.last_msg.substr(p0, nl == std::string::npos ? std::string::npos : nl - p0);
                if (!ln.empty()) lines.push_back(ln);
                if (nl == std::string::npos) break;
                p0 = nl + 1;
            }
        }
        const cv::Scalar c = s.last_pass ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255);
        for (size_t k = 0; k < lines.size(); ++k)
        {
            int base = 0; const double fs = 0.7; const int th = 2;
            cv::Size ts = cv::getTextSize(lines[k], cv::FONT_HERSHEY_SIMPLEX, fs, th, &base);
            int x = std::max(10, frame_size.width - ts.width - 12);
            int y = 30 + static_cast<int>(k) * 30;
            draw_text(ctx, lines[k].c_str(), cv::Point(x, y), c, fs, th);
        }
    }
}

/* ============================================================================
 * logic_wafer2 —— 晶圆擦拭轨迹分段可视化 (src/logic/visualize_trajectories.py 移植)
 *
 * 模型类别: 0=cleanwiper(刮片), 1=wafer(晶圆盘) —— 与 logic_wafer 相同。
 *
 * 每帧流程(把 Python 离线脚本改为实时增量):
 *   ① 最高分 wafer 框 → 5 步法提取晶圆椭圆(裁剪→灰度→高斯模糊→暗区二值→开闭运算→
 *      碎片轮廓→凸包→fitEllipse), 面积突变剔除 + EMA 时序平滑(同脚本 extract_wafer_ellipse);
 *   ② 最高分刮片框中心追加到轨迹(t = 距本轮起点秒数);
 *   ③ 覆盖率: 500×500 归一化掩码累计 圆点+连线(刮片宽度固定16px), 单位圆内占比
 *      (同 compute_coverage_from_ellipse; 脚本用全视频中值椭圆做基准, 实时版用当前平滑椭圆);
 *   ④ 轨迹分段(segment_trajectory 逐行移植): 时间间隙预切(>=0.15s) → 折返率判
 *      横擦(linear)/环擦(spiral) → 长直线段按重平滑 PCA 角度突变再切 → 相邻短段合并 →
 *      边界 spiral 复判; 每新增 5 个点重算一次(脚本为全轨迹离线一次性计算);
 *   ⑤ 叠加: 红色虚线椭圆+「覆盖基准」 → 绿色半透明覆盖带 → 当前点(段色圆点+白圈) →
 *      最近 3s 分段轨迹 → 顶部图例(覆盖面积/刮片宽度/横擦·竖擦·环擦 模板槽位 ▶✓○)。
 *
 * 与脚本的差异(实时化所需):
 *   - 脚本跑完整段视频后回放绘制; 本逻辑边采集边算, 分段结果随轨迹增长而细化;
 *   - 覆盖带视觉效果用全程轨迹半透明粗折线近似(脚本是掩码反变形回椭圆内, 视觉接近);
 *   - 刮片消失超 5s 视为一轮结束并自动复位(脚本单视频离线, 无需复位; 循环流必须有);
 *   - YOLO 检测框由框架固定绘制(脚本不画), 此处仅改色: 刮片绿/晶圆灰。
 *
 * 参数暂写死(来源: 脚本命令行默认值 + config.yaml 中脚本实际用到的项):
 *   dark_threshold=105(--dark-thresh 默认; 注: config.yaml roi.dark_threshold=80 脚本并未使用),
 *   smooth_alpha=0.05(roi.smooth_alpha), trail_sec=3.0(--trail-sec), 刮片宽=16px(覆盖计算固定值),
 *   mask_res=500。config.yaml 的 tracking/sop 两节属其它脚本, visualize_trajectories.py 不读取;
 *   detector 节(conf=0.25/iou=0.45/imgsz=640)对应通道模型配置 → 该通道 obj_thresh 请设 0.25。
 * ==========================================================================*/

struct W2Point { float x, y; double t; };          /* t: 秒, 相对本轮起点 */

struct W2Ellipse { double cx = 0, cy = 0, a = 0, b = 0, angle = 0; }; /* (a,b)=全轴长, 同脚本 */

enum W2Phase { W2_LINEAR = 0, W2_SPIRAL = 1, W2_SHORT = 2 };
struct W2Seg { int a; int b; int phase; };         /* [a,b] 为轨迹点下标(闭区间) */

/* ---- numpy 等价小工具 ---- */
static double w2_median(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return (v.size() % 2) ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

static double w2_percentile(std::vector<double> v, double q)   /* np.percentile 线性插值 */
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = q / 100.0 * (double)(v.size() - 1);
    const size_t lo = (size_t)idx;
    if (lo + 1 >= v.size()) return v.back();
    return v[lo] + (idx - (double)lo) * (v[lo + 1] - v[lo]);
}

/* 滑动窗折返率: 步进方向变化 >150° 记一次折返, 窗口 [t-w/2, t+w/2] 内 折返数/时长。
 * 窗内点数 <5 或时长 <=0.3s → NAN(无效)。对应脚本 compute_reversal_rate(双指针等价加速)。 */
static std::vector<double> w2_reversal_rate(const W2Point *p, int n, double window_sec)
{
    std::vector<double> rate((size_t)std::max(n, 0), NAN);
    if (n <= 1) return rate;
    std::vector<unsigned char> rev((size_t)n, 0);
    double last_angle = NAN;
    for (int i = 1; i < n; ++i)
    {
        const double dx = p[i].x - p[i - 1].x, dy = p[i].y - p[i - 1].y;
        if (std::fabs(dx) < 1.0 && std::fabs(dy) < 1.0) continue;
        const double ang = std::atan2(dy, dx) * 180.0 / CV_PI;
        if (!std::isnan(last_angle))
        {
            double diff = std::fabs(ang - last_angle);
            if (diff > 180.0) diff = 360.0 - diff;
            if (diff > 150.0) rev[i] = 1;
        }
        last_angle = ang;
    }
    int lo = 0, hi = 0, revs = 0;                  /* [lo, hi) 滑动窗, t 单调递增 */
    for (int i = 0; i < n; ++i)
    {
        const double wl = p[i].t - window_sec * 0.5, wh = p[i].t + window_sec * 0.5;
        while (hi < n && p[hi].t <= wh) { revs += rev[hi]; ++hi; }
        while (lo < hi && p[lo].t < wl) { revs -= rev[lo]; ++lo; }
        if (hi - lo >= 5)
        {
            const double dur = p[hi - 1].t - p[lo].t;
            if (dur > 0.3) rate[i] = (double)revs / dur;
        }
    }
    return rate;
}

/* 每点 ±half_window 小窗 PCA 主轴角(mod 180°), 再做 kernel 点零填充滑动平均
 * (同 np.convolve mode='same')。输出仅含有效点(窗内>=5点)的 (t, 平滑角)。
 * 对应脚本 local_pca_angle_smoothed。 */
static void w2_pca_angle_smoothed(const W2Point *p, int n, int half_window, int kernel,
                                  std::vector<double> &out_ts, std::vector<double> &out_ang)
{
    out_ts.clear(); out_ang.clear();
    std::vector<double> ang;
    for (int i = 0; i < n; ++i)
    {
        const int lo = std::max(0, i - half_window), hi = std::min(n, i + half_window + 1);
        const int m = hi - lo;
        if (m < 5) continue;
        double cx = 0, cy = 0;
        for (int j = lo; j < hi; ++j) { cx += p[j].x; cy += p[j].y; }
        cx /= m; cy /= m;
        double sxx = 0, syy = 0, sxy = 0;
        for (int j = lo; j < hi; ++j)
        {
            const double u = p[j].x - cx, v = p[j].y - cy;
            sxx += u * u; syy += v * v; sxy += u * v;
        }
        double a = 0.5 * std::atan2(2.0 * sxy, sxx - syy) * 180.0 / CV_PI;
        while (a >= 90.0) a -= 180.0;
        while (a < -90.0) a += 180.0;
        out_ts.push_back(p[i].t);
        ang.push_back(a);
    }
    const int vn = (int)ang.size(), half = kernel / 2;
    out_ang.assign((size_t)vn, 0.0);
    for (int i = 0; i < vn; ++i)
    {
        double sum = 0;
        for (int k = -half; k <= half; ++k)
        {
            const int j = i + k;
            if (j >= 0 && j < vn) sum += ang[j];
        }
        out_ang[i] = sum / (double)kernel;         /* 边缘零填充: 恒除 kernel, 同 np.convolve */
    }
}

/* 轨迹分段 —— 脚本 segment_trajectory 逐行移植。
 * 输入按时间升序的检出点; 返回按起点排序的 (a,b,phase) 段列表。 */
static std::vector<W2Seg> w2_segment_trajectory(const std::vector<W2Point> &pts)
{
    const int n = (int)pts.size();
    std::vector<W2Seg> out;
    if (n <= 0) return out;
    if (n < 15) { out.push_back({0, n - 1, W2_LINEAR}); return out; }

    /* ---- ① 按细微时间间隙(>=0.15s)预切 ---- */
    std::vector<std::pair<int, int>> time_segs;
    int seg_start = 0;
    for (int i = 1; i < n; ++i)
    {
        if (pts[i].t - pts[i - 1].t >= 0.15)
        {
            if (i - 1 - seg_start >= 3) time_segs.push_back({seg_start, i - 1});
            seg_start = i;
        }
    }
    if (n - 1 - seg_start >= 3) time_segs.push_back({seg_start, n - 1});
    if (time_segs.empty()) time_segs.push_back({0, n - 1});

    std::vector<W2Seg> all;

    for (const auto &tspan : time_segs)
    {
        const int ts_a = tspan.first, ts_b = tspan.second;
        const W2Point *sub = pts.data() + ts_a;
        const int sub_n = ts_b - ts_a + 1;

        if (sub_n < 10) { all.push_back({ts_a, ts_b, W2_SHORT}); continue; }

        /* ---- ② 折返率判 linear/spiral ---- */
        const std::vector<double> rate = w2_reversal_rate(sub, sub_n, 1.5);
        std::vector<double> valid_rates;
        int first_valid = -1, last_valid = -1;
        for (int i = 0; i < sub_n; ++i)
            if (!std::isnan(rate[i]))
            {
                valid_rates.push_back(rate[i]);
                if (first_valid < 0) first_valid = i;
                last_valid = i;
            }
        if ((int)valid_rates.size() < 10) { all.push_back({ts_a, ts_b, W2_LINEAR}); continue; }

        /* 自适应阈值: 90 分位的 30%, 下限 0.5 */
        const double thr = std::max(0.5, 0.30 * w2_percentile(valid_rates, 90.0));

        /* 0.8s 分块取中值折返率 */
        struct W2Block { double rate; int start, end, phase; };
        std::vector<W2Block> blocks;
        const double block_sec = 0.8;
        for (double t0 = sub[first_valid].t; t0 < sub[last_valid].t; t0 += block_sec)
        {
            std::vector<double> rs;
            int bs = -1, be = -1;
            for (int j = 0; j < sub_n; ++j)
            {
                if (std::isnan(rate[j])) continue;
                if (sub[j].t >= t0 && sub[j].t < t0 + block_sec)
                {
                    rs.push_back(rate[j]);
                    if (bs < 0) bs = j;
                    be = j;
                }
            }
            if ((int)rs.size() >= 3) blocks.push_back({w2_median(rs), bs, be, 0});
        }

        if ((int)blocks.size() < 2)
        {
            const int ph = (!blocks.empty() && blocks[0].rate > thr) ? W2_LINEAR : W2_SPIRAL;
            all.push_back({ts_a, ts_b, ph});
            continue;
        }
        for (auto &b : blocks) b.phase = (b.rate >= thr) ? W2_LINEAR : W2_SPIRAL;

        /* 相位变化处分界 → 块组 → rr_segs */
        std::vector<int> bounds;
        bounds.push_back(0);
        for (int i = 1; i < (int)blocks.size(); ++i)
            if (blocks[i].phase != blocks[i - 1].phase) bounds.push_back(i);
        bounds.push_back((int)blocks.size());

        std::vector<W2Seg> rr_segs;
        for (int j = 0; j + 1 < (int)bounds.size(); ++j)
        {
            const int bs = bounds[j], be = bounds[j + 1] - 1;
            if (be < bs) continue;
            const int seg_s = blocks[bs].start, seg_e = blocks[be].end;
            if (sub[seg_e].t - sub[seg_s].t >= 0.3)
                rr_segs.push_back({seg_s, seg_e, blocks[bs].phase});
        }

        /* ---- ③ 长 linear 段(>3s)按重平滑 PCA 角度突变再切 ---- */
        std::vector<double> v_ts, v_ang;
        w2_pca_angle_smoothed(sub, sub_n, 12, 15, v_ts, v_ang);

        for (const auto &rs : rr_segs)
        {
            const int a = rs.a, b = rs.b;
            const double seg_dur = sub[b].t - sub[a].t;
            if (rs.phase != W2_LINEAR || seg_dur < 3.0)
            {
                all.push_back({ts_a + a, ts_a + b, rs.phase});
                continue;
            }

            std::vector<double> sa, st;
            for (size_t k = 0; k < v_ts.size(); ++k)
                if (v_ts[k] >= sub[a].t && v_ts[k] <= sub[b].t) { sa.push_back(v_ang[k]); st.push_back(v_ts[k]); }
            const int n_ang = (int)sa.size();
            if (n_ang < 15)
            {
                all.push_back({ts_a + a, ts_a + b, rs.phase});
                continue;
            }

            /* 找最大持续角度漂移(跳过段首 25% 和段尾 15%) */
            const double search_lo = sub[a].t + std::max(1.8, 0.25 * seg_dur);
            const double search_hi = sub[b].t - std::max(0.5, 0.15 * seg_dur);
            const int half_w = std::max(3, n_ang / 6);
            double best_shift = 0.0, best_split_t = NAN;
            for (int i = half_w; i < n_ang - half_w; ++i)
            {
                if (st[i] < search_lo || st[i] > search_hi) continue;
                const double before = w2_median(std::vector<double>(sa.begin() + std::max(0, i - half_w), sa.begin() + i));
                const double after  = w2_median(std::vector<double>(sa.begin() + i, sa.begin() + std::min(n_ang, i + half_w)));
                double diff = std::fabs(after - before);
                if (diff > 180.0) diff = 360.0 - diff;
                if (diff > best_shift && diff >= 30.0) { best_shift = diff; best_split_t = st[i]; }
            }

            bool split_done = false;
            if (!std::isnan(best_split_t))
            {
                int arg = 0;
                double bestd = 1e18;
                for (int j = 0; j < sub_n; ++j)
                {
                    const double d = std::fabs(sub[j].t - best_split_t);
                    if (d < bestd) { bestd = d; arg = j; }
                }
                const int split_idx = a + arg;     /* a + 全子段 argmin, 与脚本行为保持一致 */
                if (split_idx > a + 8 && split_idx < b - 8)
                {
                    const double d1 = sub[split_idx].t - sub[a].t;
                    const double d2 = sub[b].t - sub[split_idx].t;
                    if (d1 >= 0.5 && d2 >= 0.5)
                    {
                        all.push_back({ts_a + a, ts_a + split_idx, W2_LINEAR});
                        all.push_back({ts_a + split_idx, ts_a + b, W2_LINEAR});
                        split_done = true;
                    }
                }
            }
            if (!split_done) all.push_back({ts_a + a, ts_a + b, rs.phase});
        }
    }

    /* ---- ④ 排序 + 相邻同相短段合并(任一侧时长 <1s 即并入) ---- */
    std::sort(all.begin(), all.end(), [](const W2Seg &x, const W2Seg &y) { return x.a < y.a; });
    std::vector<W2Seg> merged;
    for (const auto &sgm : all)
    {
        const double dur = pts[sgm.b].t - pts[sgm.a].t;
        if (!merged.empty() && merged.back().phase == sgm.phase &&
            std::min(dur, pts[merged.back().b].t - pts[merged.back().a].t) < 1.0)
            merged.back().b = sgm.b;
        else
            merged.push_back(sgm);
    }

    /* ---- ⑤ 边界 spiral 复判: a)往复直线 lin>0.70 且折返率>0.5; b)平推扫掠 lin>0.55
     * 且局部角度标准差<40° 且平均步长>10px → 改判 linear ---- */
    for (auto &sgm : merged)
    {
        if (sgm.phase != W2_SPIRAL) continue;
        const int m = sgm.b - sgm.a + 1;
        if (m < 5) continue;
        const W2Point *sp = pts.data() + sgm.a;

        double cx = 0, cy = 0;
        for (int i = 0; i < m; ++i) { cx += sp[i].x; cy += sp[i].y; }
        cx /= m; cy /= m;
        double sxx = 0, syy = 0, sxy = 0;
        for (int i = 0; i < m; ++i)
        {
            const double u = sp[i].x - cx, v = sp[i].y - cy;
            sxx += u * u; syy += v * v; sxy += u * v;
        }
        sxx /= m; syy /= m; sxy /= m;
        const double trace = sxx + syy;
        if (trace <= 1.0) continue;
        const double disc = std::max(0.0, (sxx - syy) * (sxx - syy) + 4.0 * sxy * sxy);
        const double lin = ((trace + std::sqrt(disc)) / 2.0) / trace;

        const std::vector<double> r2 =
            w2_reversal_rate(sp, m, std::min(1.5, pts[sgm.b].t - pts[sgm.a].t));
        double rsum = 0;
        int rcnt = 0;
        for (double v : r2)
            if (!std::isnan(v)) { rsum += v; ++rcnt; }
        const double avg_r = rcnt ? rsum / rcnt : 0.0;

        double ang_std = 999.0, mean_step = 0.0;
        if (m >= 8)
        {
            std::vector<double> angs, steps;
            for (int i = 1; i < m; ++i)
            {
                const double dx = sp[i].x - sp[i - 1].x, dy = sp[i].y - sp[i - 1].y;
                const double d = std::hypot(dx, dy);
                if (d > 3.0)
                {
                    steps.push_back(d);
                    double adeg = std::atan2(dy, dx) * 180.0 / CV_PI;
                    while (adeg >= 90.0) adeg -= 180.0;
                    while (adeg < -90.0) adeg += 180.0;
                    angs.push_back(adeg);
                }
            }
            if (!angs.empty())
            {
                double am = 0;
                for (double v : angs) am += v;
                am /= angs.size();
                double var = 0;
                for (double v : angs) var += (v - am) * (v - am);
                ang_std = std::sqrt(var / angs.size());
            }
            if (!steps.empty())
            {
                double sm = 0;
                for (double v : steps) sm += v;
                mean_step = sm / steps.size();
            }
        }
        if (lin > 0.70 && avg_r > 0.5) sgm.phase = W2_LINEAR;
        else if (lin > 0.55 && ang_std < 40.0 && mean_step > 10.0) sgm.phase = W2_LINEAR;
    }
    return merged;
}

/* 0°/180° 周期上的角度 EMA 平滑(同脚本 _smooth_angle_deg) */
static double w2_smooth_angle(double prev, double cur, double alpha)
{
    double a = cur;
    while (a - prev > 90.0)  a -= 180.0;
    while (a - prev < -90.0) a += 180.0;
    double r = std::fmod(alpha * a + (1.0 - alpha) * prev, 180.0);
    if (r < 0) r += 180.0;
    return r;
}

/* 晶圆椭圆 5 步提取 + 面积突变剔除 + EMA 平滑(同脚本 extract_wafer_ellipse)。
 * e 传入上次结果、传出更新值; 本帧提取失败时保持 e 不变。返回是否已有可用椭圆。 */
static bool w2_extract_ellipse(const cv::Mat &frame, const cv::Rect &bbox,
                               int dark_threshold, double alpha,
                               bool have_last, W2Ellipse &e)
{
    const int W = frame.cols, H = frame.rows, pad = 10;
    const int x1 = std::max(0, bbox.x - pad);
    const int y1 = std::max(0, bbox.y - pad);
    const int x2 = std::min(W, bbox.x + bbox.width + pad);
    const int y2 = std::min(H, bbox.y + bbox.height + pad);
    if (x2 <= x1 || y2 <= y1) return have_last;

    cv::Mat gray, blur, th;
    cv::cvtColor(frame(cv::Rect(x1, y1, x2 - x1, y2 - y1)), gray, cv::COLOR_BGR2GRAY);
    cv::GaussianBlur(gray, blur, cv::Size(5, 5), 0);
    cv::threshold(blur, th, dark_threshold, 255, cv::THRESH_BINARY_INV);
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(7, 7));
    cv::morphologyEx(th, th, cv::MORPH_OPEN, kernel);
    cv::morphologyEx(th, th, cv::MORPH_CLOSE, kernel);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(th, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    std::vector<cv::Point> all_pts;                /* 暗区碎片合并(回填全图坐标) */
    for (const auto &c : contours)
        if (cv::contourArea(c) > 100.0)
            for (const auto &q : c) all_pts.emplace_back(q.x + x1, q.y + y1);
    if ((int)all_pts.size() < 5) return have_last;

    std::vector<cv::Point> hull;                   /* 凸包桥接指缝遮挡 */
    cv::convexHull(all_pts, hull);
    if ((int)hull.size() < 5) return have_last;

    cv::RotatedRect fit;
    try { fit = cv::fitEllipse(hull); }
    catch (const cv::Exception &) { return have_last; }

    const W2Ellipse cur{fit.center.x, fit.center.y, fit.size.width, fit.size.height, fit.angle};
    if (!have_last) { e = cur; return true; }

    const double cur_area  = std::max(cur.a, 0.0) * std::max(cur.b, 0.0);
    const double prev_area = std::max(e.a, 0.0) * std::max(e.b, 0.0);
    if (prev_area > 0 && (cur_area / prev_area > 1.3 || cur_area / prev_area < 0.7))
        return true;                               /* 面积突变 → 拒绝, 保留上一帧 */

    e.angle = w2_smooth_angle(e.angle, cur.angle, alpha);
    e.cx = alpha * cur.cx + (1.0 - alpha) * e.cx;
    e.cy = alpha * cur.cy + (1.0 - alpha) * e.cy;
    e.a  = alpha * cur.a  + (1.0 - alpha) * e.a;
    e.b  = alpha * cur.b  + (1.0 - alpha) * e.b;
    return true;
}

struct Wafer2State
{
    /* 轨迹(仅检出帧; t=距本轮起点秒) */
    std::vector<W2Point> pts;
    bool     active = false;
    uint64_t t0_ms = 0;
    uint64_t last_seen_ms = 0;

    /* 晶圆椭圆(EMA 平滑, 同时充当覆盖基准) */
    bool      have_ellipse = false;
    W2Ellipse ellipse;
    bool      have_bbox = false;
    cv::Rect  last_bbox;

    /* 覆盖率: 归一化掩码 + 单位圆基准 */
    cv::Mat   cov_mask, unit_mask;
    int       wafer_pixels = 0;
    bool      have_prev_mpt = false;
    cv::Point prev_mpt;
    float     coverage = 0.0f;

    /* 分段缓存(每加 N 点重算一次) */
    std::vector<W2Seg> segs;
    std::vector<int>   seg_of;                     /* 点下标 → 段下标, -1=未入段 */
    int                pts_at_seg = 0;
    int                dbg_seg_cnt = 0;
};

static void logic_wafer2(ChannelContext *ctx)
{
    if (!ctx || !ctx->results) return;
    if (!ctx->frame || ctx->frame->empty())
    {
        draw_text(ctx, "wafer2: frame empty", cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.6, 2);
        return;
    }

    if (!*ctx->state) *ctx->state = std::make_shared<Wafer2State>();
    auto &s = *std::static_pointer_cast<Wafer2State>(*ctx->state);

    /* ---- 参数(脚本默认值/config.yaml, 暂写死; 出处见函数头注释) ---- */
    constexpr int      DARK_THRESHOLD = 105;       /* --dark-thresh 默认 */
    constexpr double   EMA_ALPHA      = 0.05;      /* config.yaml roi.smooth_alpha */
    constexpr double   TRAIL_SEC      = 3.0;       /* --trail-sec 默认 */
    constexpr double   SCRAPER_W_PX   = 16.0;      /* 覆盖计算固定刮片宽度(模型坐标 px) */
    constexpr int      MASK_RES       = 500;       /* 归一化覆盖掩码分辨率 */
    constexpr uint64_t RESET_MS       = 5000;      /* 刮片消失超此时长 → 一轮结束复位 */
    constexpr int      MAX_PTS        = 4000;      /* 轨迹点上限(防循环流无限增长) */
    constexpr int      SEG_EVERY_PTS  = 5;         /* 每新增 N 点重算一次分段 */

    static const cv::Scalar W2_COLORS[5] = {       /* 分段调色板(BGR), 同脚本 SEGMENT_COLORS */
        cv::Scalar(0, 255, 0),   cv::Scalar(255, 165, 0), cv::Scalar(255, 0, 255),
        cv::Scalar(0, 255, 255), cv::Scalar(255, 80, 80),
    };

    const uint64_t now = ctx->timestamp_ms;

    if (s.cov_mask.empty())                        /* 掩码/单位圆惰性初始化 */
    {
        s.cov_mask  = cv::Mat::zeros(MASK_RES, MASK_RES, CV_8UC1);
        s.unit_mask = cv::Mat::zeros(MASK_RES, MASK_RES, CV_8UC1);
        cv::circle(s.unit_mask, cv::Point(MASK_RES / 2, MASK_RES / 2), MASK_RES / 2, cv::Scalar(255), -1);
        s.wafer_pixels = cv::countNonZero(s.unit_mask);
    }

    /* ---- ① 最高分 刮片(class 0) / 晶圆(class 1) ---- */
    AlgoResult *scraper = nullptr, *wafer = nullptr;
    for (auto &r : *ctx->results)
    {
        if (r.class_id == 0) { if (!scraper || r.score > scraper->score) scraper = &r; }
        else if (r.class_id == 1) { if (!wafer || r.score > wafer->score) wafer = &r; }
    }
    if (scraper) scraper->box_color = cv::Scalar(0, 255, 0);
    if (wafer)   wafer->box_color   = cv::Scalar(160, 160, 160);

    /* ---- ② 晶圆椭圆(当前框, 缺检沿用上次框, 同脚本 current or last_wafer_bbox) ---- */
    if (wafer) { s.last_bbox = wafer->box; s.have_bbox = true; }
    if (s.have_bbox)
        s.have_ellipse = w2_extract_ellipse(*ctx->frame, s.last_bbox, DARK_THRESHOLD,
                                            EMA_ALPHA, s.have_ellipse, s.ellipse);

    /* ---- ③ 一轮复位 ---- */
    if (s.active && !scraper && now - s.last_seen_ms > RESET_MS)
    {
        s.pts.clear(); s.segs.clear(); s.seg_of.clear();
        s.pts_at_seg = 0;
        s.cov_mask.setTo(0);
        s.have_prev_mpt = false;
        s.coverage = 0.0f;
        s.active = false;
    }

    /* ---- ④ 轨迹追加 + 覆盖累计(圆点 r=w/3 + 连线 thick=0.7w, 同脚本) ---- */
    if (scraper)
    {
        if (!s.active) { s.active = true; s.t0_ms = now; }
        s.last_seen_ms = now;
        if ((int)s.pts.size() < MAX_PTS)
        {
            const cv::Point c = scraper->box_center();
            s.pts.push_back({(float)c.x, (float)c.y, (double)(now - s.t0_ms) / 1000.0});

            if (s.have_ellipse)
            {
                const double rx = std::max(s.ellipse.a / 2.0, 1.0);
                const double ry = std::max(s.ellipse.b / 2.0, 1.0);
                const int mx = std::min(std::max((int)((c.x - s.ellipse.cx) / rx * (MASK_RES / 2.0) + MASK_RES / 2.0), 0), MASK_RES - 1);
                const int my = std::min(std::max((int)((c.y - s.ellipse.cy) / ry * (MASK_RES / 2.0) + MASK_RES / 2.0), 0), MASK_RES - 1);
                const cv::Point mpt(mx, my);
                const double w_mask = SCRAPER_W_PX / rx * (MASK_RES / 2.0);
                cv::circle(s.cov_mask, mpt, std::max(1, (int)(w_mask / 3.0)), cv::Scalar(255), -1);
                if (s.have_prev_mpt)
                    cv::line(s.cov_mask, s.prev_mpt, mpt, cv::Scalar(255), std::max(1, (int)(w_mask * 0.7)));
                s.prev_mpt = mpt;
                s.have_prev_mpt = true;

                cv::Mat inter;
                cv::bitwise_and(s.cov_mask, s.unit_mask, inter);
                s.coverage = (float)cv::countNonZero(inter) / (float)std::max(s.wafer_pixels, 1);
            }
        }
    }

    /* ---- ⑤ 分段(节流重算; 脚本为离线一次性) ---- */
    if ((int)s.pts.size() >= 2 &&
        (s.segs.empty() || (int)s.pts.size() - s.pts_at_seg >= SEG_EVERY_PTS))
    {
        s.segs = w2_segment_trajectory(s.pts);
        s.seg_of.assign(s.pts.size(), -1);
        for (int k = 0; k < (int)s.segs.size(); ++k)
            for (int i = s.segs[k].a; i <= s.segs[k].b && i < (int)s.seg_of.size(); ++i)
                s.seg_of[i] = k;
        s.pts_at_seg = (int)s.pts.size();

        if ((int)s.segs.size() != s.dbg_seg_cnt)
        {
            s.dbg_seg_cnt = (int)s.segs.size();
            printf("[wafer2] ch=%d pts=%zu segs=%d cov=%.1f%%\n",
                   ctx->chnId, s.pts.size(), s.dbg_seg_cnt, s.coverage * 100.0f);
        }
    }

    /* 节流重算间隙新加的点暂按最后一段着色, 避免当前点/轨迹闪烁 */
    auto seg_idx_of = [&](int i) -> int {
        if (i >= 0 && i < (int)s.seg_of.size()) return s.seg_of[i];
        return s.segs.empty() ? -1 : (int)s.segs.size() - 1;
    };

    /* ---- ⑥ 叠加绘制(顺序同脚本: 椭圆 → 覆盖 → 当前点 → 轨迹 → 图例) ---- */
    if (s.have_ellipse)
    {
        std::vector<cv::Point> poly;                /* 红色虚线椭圆: 隔段画线 */
        cv::ellipse2Poly(cv::Point((int)std::lround(s.ellipse.cx), (int)std::lround(s.ellipse.cy)),
                         cv::Size((int)std::lround(s.ellipse.a / 2.0), (int)std::lround(s.ellipse.b / 2.0)),
                         (int)std::lround(s.ellipse.angle), 0, 360, 5, poly);
        for (size_t k = 0; k + 1 < poly.size(); k += 2)
            draw_line(ctx, poly[k], poly[k + 1], cv::Scalar(0, 0, 255), 2);
        draw_text(ctx, "覆盖基准",
                  cv::Point((int)std::lround(s.ellipse.cx - s.ellipse.a / 2.0),
                            std::max(15, (int)std::lround(s.ellipse.cy + s.ellipse.b / 2.0) + 18)),
                  cv::Scalar(0, 0, 255), 0.4, 1);
    }
    else
        draw_text(ctx, "wafer2: waiting for wafer", cv::Point(20, 64), cv::Scalar(0, 165, 255), 0.5, 1);

    if (s.pts.size() >= 2)                          /* 覆盖带: 全程轨迹半透明绿粗折线 */
    {
        std::vector<cv::Point> tp;
        tp.reserve(s.pts.size());
        for (const auto &q : s.pts) tp.emplace_back((int)q.x, (int)q.y);
        draw_polyline(ctx, tp, cv::Scalar(0, 200, 0), (int)SCRAPER_W_PX, 0.25, false);
    }

    const double t_now = s.active ? (double)(now - s.t0_ms) / 1000.0 : 0.0;

    if (scraper && !s.pts.empty())                  /* 当前点: 段色实心圆 + 白圈 */
    {
        const int k = seg_idx_of((int)s.pts.size() - 1);
        if (k >= 0)
        {
            const cv::Point dot((int)s.pts.back().x, (int)s.pts.back().y);
            draw_circle(ctx, dot, 7, W2_COLORS[k % 5], -1);
            draw_circle(ctx, dot, 9, cv::Scalar(255, 255, 255), 1);
        }
    }

    for (int i = (int)s.pts.size() - 1; i >= 1; --i)   /* 最近 TRAIL_SEC 秒分段轨迹 */
    {
        if (s.pts[i].t < t_now - TRAIL_SEC) break;
        const int k = seg_idx_of(i);
        if (k < 0 || k != seg_idx_of(i - 1)) continue; /* 跨段不连线(同脚本) */
        draw_line(ctx, cv::Point((int)s.pts[i - 1].x, (int)s.pts[i - 1].y),
                  cv::Point((int)s.pts[i].x, (int)s.pts[i].y), W2_COLORS[k % 5], 2);
    }

    /* ---- ⑦ 顶部图例(单行, 覆盖/宽度 + 横擦/竖擦/环擦) ---- */
    const cv::Scalar ORANGE(0, 80, 230), GREEN(0, 160, 0);
    char buf[64];

    draw_text(ctx, "覆盖:", cv::Point(5, 18), ORANGE, 0.5, 2);
    snprintf(buf, sizeof(buf), "%.1f%%", s.coverage * 100.0f);
    draw_text(ctx, buf, cv::Point(52, 18), GREEN, 0.5, 2);
    draw_text(ctx, "宽:", cv::Point(102, 18), ORANGE, 0.5, 2);
    snprintf(buf, sizeof(buf), "%.0fpx", SCRAPER_W_PX);
    draw_text(ctx, buf, cv::Point(122, 18), GREEN, 0.5, 2);

    /* 模板槽位(同脚本 TEMPLATE): 检出的 linear 段依次填 横擦/竖擦, spiral 段填 环擦 */
    struct Slot { const char *name; bool detected; double t0, t1; };
    Slot slots[3] = {{"横擦", false, 0, 0}, {"竖擦", false, 0, 0}, {"环擦", false, 0, 0}};
    {
        const W2Seg *lin[2] = {nullptr, nullptr};
        const W2Seg *spi = nullptr;
        for (const auto &sgm : s.segs)
        {
            if (sgm.phase == W2_LINEAR) { if (!lin[0]) lin[0] = &sgm; else if (!lin[1]) lin[1] = &sgm; }
            else if (sgm.phase == W2_SPIRAL) { if (!spi) spi = &sgm; }
        }
        const W2Seg *pick[3] = {lin[0], lin[1], spi};
        for (int i = 0; i < 3; ++i)
            if (pick[i])
            {
                slots[i].detected = true;
                slots[i].t0 = s.pts[pick[i]->a].t;
                slots[i].t1 = s.pts[pick[i]->b].t;
            }
    }
    /* 每个槽: label(~38px) + value(~60px) = ~98px; 3槽从 x=163 起 */
    static const int slot_x[3] = {163, 290, 417};
    for (int i = 0; i < 3; ++i)
    {
        snprintf(buf, sizeof(buf), "%s:", slots[i].name);
        draw_text(ctx, buf, cv::Point(slot_x[i], 18), ORANGE, 0.5, 2);
        if (slots[i].detected && t_now >= slots[i].t0 && t_now <= slots[i].t1)
            snprintf(buf, sizeof(buf), "%.1fs ▶", t_now - slots[i].t0);
        else if (slots[i].detected && t_now > slots[i].t1)
            snprintf(buf, sizeof(buf), "%.1fs ✓", slots[i].t1 - slots[i].t0);
        else
            snprintf(buf, sizeof(buf), "○");
        draw_text(ctx, buf, cv::Point(slot_x[i] + 38, 18), GREEN, 0.5, 2);
    }
}

/* logic_fall_detect —— 人员跌倒检测
 *
 * 优先使用 yolov8_pose 的人体关键点判断人体是否接近横躺；如果当前模型没有关键点,
 * 则退回到 person 检测框宽高比(width / height)判断。满足条件持续 fall_dwell_sec
 * 后报警, 并按 fall_cooldown_sec 限频上报服务器。 */
struct FallDetectState
{
    uint64_t fall_start_ms = 0;
    uint64_t last_upload_ms = 0;
    uint64_t wave_start_ms = 0;
    uint64_t last_wave_upload_ms = 0;
    float last_wrist_x = -1.0f;
    int wave_dir = 0;
    int wave_swings = 0;
    bool alarm_latched = false;
    bool wave_latched = false;
};

static bool fall_pose_like(const AlgoResult &r, float ratio_thresh)
{
    if (r.keypoints.size() < 17)
        return false;

    auto valid = [](const cv::Point2f &p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && p.x > 1.0f && p.y > 1.0f;
    };
    auto mid = [](const cv::Point2f &a, const cv::Point2f &b) {
        return cv::Point2f((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    };

    const cv::Point2f &ls = r.keypoints[5];
    const cv::Point2f &rs = r.keypoints[6];
    const cv::Point2f &lh = r.keypoints[11];
    const cv::Point2f &rh = r.keypoints[12];
    if (!valid(ls) || !valid(rs) || !valid(lh) || !valid(rh))
        return false;

    cv::Point2f shoulder = mid(ls, rs);
    cv::Point2f hip = mid(lh, rh);
    float body_dx = std::fabs(shoulder.x - hip.x);
    float body_dy = std::fabs(shoulder.y - hip.y);
    float box_ratio = r.box.height > 0 ? (float)r.box.width / (float)r.box.height : 0.0f;

    bool torso_horizontal = body_dx > body_dy * 1.25f;
    bool box_horizontal = box_ratio >= ratio_thresh;
    return torso_horizontal || box_horizontal;
}

static bool wave_pose_candidate(const AlgoResult &r, cv::Point2f *wrist_out)
{
    if (r.keypoints.size() < 17)
        return false;

    auto valid = [](const cv::Point2f &p) {
        return std::isfinite(p.x) && std::isfinite(p.y) && p.x > 1.0f && p.y > 1.0f;
    };

    const int pairs[2][3] = {
        {5, 7, 9},   /* left shoulder, elbow, wrist */
        {6, 8, 10},  /* right shoulder, elbow, wrist */
    };

    cv::Point2f best_wrist;
    bool found = false;
    float best_raise = 0.0f;
    for (const auto &p : pairs)
    {
        const cv::Point2f &shoulder = r.keypoints[p[0]];
        const cv::Point2f &elbow = r.keypoints[p[1]];
        const cv::Point2f &wrist = r.keypoints[p[2]];
        if (!valid(shoulder) || !valid(elbow) || !valid(wrist))
            continue;

        float raise = shoulder.y - wrist.y;
        float min_raise = std::max(12.0f, r.box.height * 0.08f);
        bool hand_above_shoulder = raise >= min_raise;
        bool elbow_reasonable = elbow.y <= shoulder.y + r.box.height * 0.25f;
        if (hand_above_shoulder && elbow_reasonable && raise > best_raise)
        {
            best_raise = raise;
            best_wrist = wrist;
            found = true;
        }
    }

    if (found && wrist_out)
        *wrist_out = best_wrist;
    return found;
}

static void logic_fall_detect(ChannelContext *ctx)
{
    if (!ctx || !ctx->results)
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<FallDetectState>();
    auto &s = *std::static_pointer_cast<FallDetectState>(*ctx->state);

    float ratio_thresh = ctx->config ? ctx->config->fall_ratio_thresh : 1.25f;
    float dwell_sec = ctx->config ? ctx->config->fall_dwell_sec : 2.0f;
    int cooldown_sec = ctx->config ? ctx->config->fall_cooldown_sec : 10;
    int wave_min_swings = ctx->config ? ctx->config->wave_min_swings : 2;
    float wave_window_sec = ctx->config ? ctx->config->wave_window_sec : 2.0f;
    if (ratio_thresh <= 0.1f) ratio_thresh = 1.25f;
    if (dwell_sec < 0.0f) dwell_sec = 0.0f;
    if (cooldown_sec < 1) cooldown_sec = 1;
    if (wave_min_swings < 1) wave_min_swings = 1;
    if (wave_window_sec < 0.2f) wave_window_sec = 0.2f;

    bool fall_like = false;
    bool hand_raised = false;
    int person_count = 0;
    cv::Rect best_box;
    cv::Point2f wave_wrist;
    float best_score = -1.0f;

    for (auto &r : *ctx->results)
    {
        if (r.label != "person")
            continue;
        if (!ctx->is_in_roi(r.box))
            continue;

        ++person_count;
        float box_ratio = r.box.height > 0 ? (float)r.box.width / (float)r.box.height : 0.0f;
        bool suspicious = fall_pose_like(r, ratio_thresh) || box_ratio >= ratio_thresh;
        cv::Point2f wrist;
        bool waving_pose = wave_pose_candidate(r, &wrist);

        if (suspicious)
        {
            r.box_color = cv::Scalar(0, 0, 255);
            draw_rect(ctx, r.box, cv::Scalar(0, 0, 255), 2);
            draw_text(ctx, "fall?", cv::Point(r.box.x, std::max(20, r.box.y - 8)),
                      cv::Scalar(0, 0, 255), 0.55, 2);
            if (r.score > best_score)
            {
                best_score = r.score;
                best_box = r.box;
            }
            fall_like = true;
        }
        else
        {
            r.box_color = cv::Scalar(0, 255, 0);
        }

        if (waving_pose)
        {
            hand_raised = true;
            wave_wrist = wrist;
            draw_circle(ctx, cv::Point((int)wrist.x, (int)wrist.y), 5, cv::Scalar(255, 0, 255), 2);
            draw_text(ctx, "wave?", cv::Point((int)wrist.x + 6, std::max(20, (int)wrist.y - 8)),
                      cv::Scalar(255, 0, 255), 0.5, 2);
        }
    }

    bool wave_alarm = false;
    if (hand_raised)
    {
        if (s.wave_start_ms == 0)
        {
            s.wave_start_ms = ctx->timestamp_ms;
            s.last_wrist_x = wave_wrist.x;
            s.wave_dir = 0;
            s.wave_swings = 0;
        }

        float elapsed_sec = (ctx->timestamp_ms - s.wave_start_ms) / 1000.0f;
        if (elapsed_sec > wave_window_sec && s.wave_swings < wave_min_swings)
        {
            s.wave_start_ms = ctx->timestamp_ms;
            s.last_wrist_x = wave_wrist.x;
            s.wave_dir = 0;
            s.wave_swings = 0;
            elapsed_sec = 0.0f;
        }

        float dx = wave_wrist.x - s.last_wrist_x;
        float move_thresh = 18.0f;
        int dir = (dx > move_thresh) ? 1 : ((dx < -move_thresh) ? -1 : 0);
        if (dir != 0)
        {
            if (s.wave_dir != 0 && dir != s.wave_dir)
                ++s.wave_swings;
            s.wave_dir = dir;
            s.last_wrist_x = wave_wrist.x;
        }

        wave_alarm = s.wave_swings >= wave_min_swings;
        char wave_msg[128];
        snprintf(wave_msg, sizeof(wave_msg), "Wave SOS: %d/%d %.1fs",
                 s.wave_swings, wave_min_swings, elapsed_sec);
        draw_text(ctx, wave_msg, cv::Point(20, 60),
                  wave_alarm ? cv::Scalar(255, 0, 255) : cv::Scalar(255, 128, 255),
                  0.62, 2);
    }
    else
    {
        s.wave_start_ms = 0;
        s.last_wrist_x = -1.0f;
        s.wave_dir = 0;
        s.wave_swings = 0;
        s.wave_latched = false;
    }

    if (fall_like)
    {
        if (s.fall_start_ms == 0)
            s.fall_start_ms = ctx->timestamp_ms;

        float elapsed_sec = (ctx->timestamp_ms - s.fall_start_ms) / 1000.0f;
        bool alarm_now = elapsed_sec >= dwell_sec;
        char msg[128];

        if (alarm_now)
        {
            snprintf(msg, sizeof(msg), "ALARM: FALL %.1fs", elapsed_sec);
            draw_text(ctx, msg, cv::Point(20, 30), cv::Scalar(0, 0, 255), 0.75, 2);
            if (best_box.area() > 0)
                draw_rect(ctx, best_box, cv::Scalar(0, 0, 255), 3);

            uint64_t cooldown_ms = (uint64_t)cooldown_sec * 1000ULL;
            if (!s.alarm_latched &&
                ctx->timestamp_ms - s.last_upload_ms >= cooldown_ms &&
                ctx->frame && !ctx->frame->empty())
            {
                const char *url = (ctx->config && !ctx->config->server_url.empty())
                                      ? ctx->config->server_url.c_str() : nullptr;
                alarm_uploader_enqueue(*ctx->frame, *ctx->frame, ctx->chnId, "fall_detect", url);
                s.last_upload_ms = ctx->timestamp_ms;
                s.alarm_latched = true;
            }
        }
        else
        {
            snprintf(msg, sizeof(msg), "Fall suspicious %.1f/%.1fs", elapsed_sec, dwell_sec);
            draw_text(ctx, msg, cv::Point(20, 30), cv::Scalar(0, 165, 255), 0.65, 2);
        }
    }
    else
    {
        s.fall_start_ms = 0;
        s.alarm_latched = false;

        char msg[96];
        snprintf(msg, sizeof(msg), "Fall: CLEAR person:%d", person_count);
        draw_text(ctx, msg, cv::Point(20, 30), cv::Scalar(0, 255, 0), 0.65, 2);
    }

    if (wave_alarm)
    {
        draw_text(ctx, "ALARM: WAVE SOS", cv::Point(20, 90), cv::Scalar(255, 0, 255), 0.75, 2);

        uint64_t cooldown_ms = (uint64_t)cooldown_sec * 1000ULL;
        if (!s.wave_latched &&
            ctx->timestamp_ms - s.last_wave_upload_ms >= cooldown_ms &&
            ctx->frame && !ctx->frame->empty())
        {
            const char *url = (ctx->config && !ctx->config->server_url.empty())
                                  ? ctx->config->server_url.c_str() : nullptr;
            alarm_uploader_enqueue(*ctx->frame, *ctx->frame, ctx->chnId, "wave_sos", url);
            s.last_wave_upload_ms = ctx->timestamp_ms;
            s.wave_latched = true;
        }
    }
}

void register_logic(const char *name, ChannelLogicFunc func)
{
    if (g_logic_count < MAX_LOGIC_FUNCS)
    {
        g_logic_registry[g_logic_count].name = name;
        g_logic_registry[g_logic_count].func = func;
        g_logic_count++;
    }
}

void channel_logic_init(void)
{
    g_logic_count = 0;

    register_logic("logic_default", logic_default);
    register_logic("logic_server", logic_server);
    register_logic("logic_dify", logic_dify);
    register_logic("logic_hook", logic_hook);
    register_logic("logic_roll", logic_roll);
    register_logic("logic_custom", logic_custom);
    register_logic("logic_person_alarm", logic_person_alarm);
    register_logic("logic_cross_camera", logic_cross_camera);
    register_logic("logic_dify_person_verify", logic_dify_person_verify);
    register_logic("logic_roi", logic_roi);
    register_logic("logic_wafer", logic_wafer);
    register_logic("logic_multi_roi", logic_multi_roi);
    register_logic("logic_wafer_sop", logic_wafer_sop);
    register_logic("logic_wafer2", logic_wafer2);
    register_logic("logic_fall_detect", logic_fall_detect);

    /* 新增 logic: 在此处添加 register_logic("logic_xxx", logic_xxx); 即可 */
}

void channel_logic_deinit(void)
{
    g_logic_count = 0;
    memset(g_logic_registry, 0, sizeof(g_logic_registry));
}

ChannelLogicFunc channel_logic_get(const char *name)
{
    for (int i = 0; i < g_logic_count; ++i)
    {
        if (g_logic_registry[i].name && strcmp(g_logic_registry[i].name, name) == 0)
            return g_logic_registry[i].func;
    }
    return logic_default;
}
