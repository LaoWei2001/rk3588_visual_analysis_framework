/**
 * @file logic_person_alarm.cpp
 * @brief logic_person_alarm —— ROI 内有 person 即报警可视化。
 *        person_detected / person_count 写入 PersonAlarmState(logic_tools.h),
 *        供 global_logic 读取联动。
 */
#include "logic_common.h"

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
            if (!roi_contains(ctx, r.box, ROI_ALL))   /* 不在任一 ROI(没画 ROI=整帧不设限) → 跳过 */
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

REGISTER_LOGIC("logic_person_alarm", logic_person_alarm);
