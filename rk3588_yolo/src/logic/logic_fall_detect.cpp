/**
 * @file logic_fall_detect.cpp
 * logic_fall_detect —— 人员跌倒检测
 *
 * 优先使用 yolov8_pose
 * 的人体关键点判断人体是否接近横躺；如果当前模型没有关键点, 则退回到 person
 * 检测框宽高比(width / height)判断。满足条件持续 fall_dwell_sec 后报警, 并按
 * fall_cooldown_sec 限频上报服务器。
 */
#include "logic_common.h"

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
        {5, 7, 9},  /* left shoulder, elbow, wrist */
        {6, 8, 10}, /* right shoulder, elbow, wrist */
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
    if (ratio_thresh <= 0.1f)
        ratio_thresh = 1.25f;
    if (dwell_sec < 0.0f)
        dwell_sec = 0.0f;
    if (cooldown_sec < 1)
        cooldown_sec = 1;
    if (wave_min_swings < 1)
        wave_min_swings = 1;
    if (wave_window_sec < 0.2f)
        wave_window_sec = 0.2f;

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
        if (!roi_contains(ctx, r.box, ROI_ALL))
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
            draw_text(ctx, "fall?", cv::Point(r.box.x, std::max(20, r.box.y - 8)), cv::Scalar(0, 0, 255), 0.55, 2);
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
        snprintf(wave_msg, sizeof(wave_msg), "Wave SOS: %d/%d %.1fs", s.wave_swings, wave_min_swings, elapsed_sec);
        draw_text(ctx, wave_msg, cv::Point(20, 60), wave_alarm ? cv::Scalar(255, 0, 255) : cv::Scalar(255, 128, 255),
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
            if (!s.alarm_latched && ctx->timestamp_ms - s.last_upload_ms >= cooldown_ms && ctx->frame &&
                !ctx->frame->empty())
            {
                const char *url =
                    (ctx->config && !ctx->config->server_url.empty()) ? ctx->config->server_url.c_str() : nullptr;
                alarm_uploader_enqueue(*ctx->frame, *ctx->frame, ctx->chnId, "fall_detect", report_enabled(ctx),
                                       url); /* 连了"上报配置"节点才真正发 */
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
        if (!s.wave_latched && ctx->timestamp_ms - s.last_wave_upload_ms >= cooldown_ms && ctx->frame &&
            !ctx->frame->empty())
        {
            const char *url =
                (ctx->config && !ctx->config->server_url.empty()) ? ctx->config->server_url.c_str() : nullptr;
            alarm_uploader_enqueue(*ctx->frame, *ctx->frame, ctx->chnId, "wave_sos", report_enabled(ctx),
                                   url); /* 连了"上报配置"节点才真正发 */
            s.last_wave_upload_ms = ctx->timestamp_ms;
            s.wave_latched = true;
        }
    }
}

REGISTER_LOGIC("logic_fall_detect", logic_fall_detect);
