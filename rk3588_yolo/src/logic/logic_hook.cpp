/**
 * @file logic_hook.cpp
 * @brief logic_hook —— 吊钩安全圈检测: 钩子出圈持续一段时间报警,
 * 回圈冷却后复位。 跨帧状态见 logic_tools.h 的 HookState。
 */
#include "logic_common.h"

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

    const cv::Scalar circle_color =
        (hook_outside || (in_grace && s.alarm_active)) ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
    draw_circle(ctx, safe_center, radius, circle_color, 2);

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
                                       report_enabled(ctx), /* 连了"上报配置"节点才真正发 */
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

REGISTER_LOGIC("logic_hook", logic_hook);
