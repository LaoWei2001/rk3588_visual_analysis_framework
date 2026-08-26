#include "logic/core/logic_common.h"

#include <cstdint>
#include <memory>

namespace
{

constexpr uint64_t CONTINUITY_GAP_MS = 1000;
constexpr uint64_t REPORT_RETRY_INTERVAL_MS = 1000;
constexpr uint64_t ERROR_LOG_INTERVAL_MS = 60000;

struct PersonRoiAlarmState
{
    bool presence_active = false;
    bool event_accepted = false;
    uint64_t presence_started_ms = 0;
    uint64_t last_frame_ms = 0;
    uint64_t last_report_attempt_ms = 0;
    uint64_t last_error_log_ms = 0;
};

void reset_presence(PersonRoiAlarmState &state)
{
    state.presence_active = false;
    state.event_accepted = false;
    state.presence_started_ms = 0;
    state.last_report_attempt_ms = 0;
    state.last_error_log_ms = 0;
}

bool frame_continuity_broken(const PersonRoiAlarmState &state, uint64_t now_ms)
{
    if (state.last_frame_ms == 0)
        return false;
    if (now_ms < state.last_frame_ms)
        return true;
    return now_ms - state.last_frame_ms > CONTINUITY_GAP_MS;
}

void log_report_failure_periodically(ChannelContext *ctx, PersonRoiAlarmState &state, const EventReportResult &report)
{
    if (state.last_error_log_ms != 0 && ctx->timestamp_ms - state.last_error_log_ms < ERROR_LOG_INTERVAL_MS)
        return;

    state.last_error_log_ms = ctx->timestamp_ms;
    fprintf(stderr, "[logic_person_roi_alarm][ch%02d] event not created: %s (%s)\n", ctx->chnId,
            event_report_status_name(report.status), report.detail.c_str());
}

void logic_person_roi_alarm(ChannelContext *ctx)
{
    if (!ctx || !ctx->state)
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<PersonRoiAlarmState>();
    PersonRoiAlarmState &state = *std::static_pointer_cast<PersonRoiAlarmState>(*ctx->state);

    const uint64_t now_ms = ctx->timestamp_ms;
    if (frame_continuity_broken(state, now_ms))
        reset_presence(state);
    state.last_frame_ms = now_ms;

    const std::string roi_name = ctx->param_string("roi_name");
    const std::string target_label = ctx->param_string("target_label");
    const RoiZone *roi = roi_name.empty() ? nullptr : ctx->roi_by_name(roi_name.c_str());
    if (!ctx->results || target_label.empty() || !roi || roi->polygon.size() < 3)
    {
        reset_presence(state);
        return;
    }

    const int roi_index = roi_find(ctx, roi_name.c_str());
    const int target_count = roi_count_target(ctx, target_label.c_str(), roi_index);
    if (target_count <= 0)
    {
        reset_presence(state);
        return;
    }

    if (!state.presence_active)
    {
        state.presence_active = true;
        state.presence_started_ms = now_ms;
    }

    const double dwell_seconds = ctx->param_float("dwell_seconds");
    const uint64_t dwell_threshold_ms = static_cast<uint64_t>(dwell_seconds * 1000.0);
    const uint64_t elapsed_ms = now_ms - state.presence_started_ms;
    if (elapsed_ms < dwell_threshold_ms)
        return;

    const DrawCommand::Target alarm_targets =
        static_cast<DrawCommand::Target>(DrawCommand::DISPLAY | DrawCommand::IMAGE);
    draw_polyline(ctx, roi->polygon, cv::Scalar(0, 0, 255), 4, 1.0, true, alarm_targets);

    if (state.event_accepted)
        return;
    if (state.last_report_attempt_ms != 0 && now_ms - state.last_report_attempt_ms < REPORT_RETRY_INTERVAL_MS)
        return;

    state.last_report_attempt_ms = now_ms;
    EventRequest request;
    request.event_type = "person_roi_alarm";
    request.message = "人员在警戒区持续停留: " + roi_name;
    request.merge_mode = EventMergeMode::NEVER;
    request.fields = {
        event_field("roi_name", roi_name),
        event_field("target_label", target_label),
        event_field("target_count", target_count),
        event_field("dwell_seconds", static_cast<double>(elapsed_ms) / 1000.0),
    };

    const EventReportResult report = report_event(ctx, request);
    if (report.accepted())
    {
        state.event_accepted = true;
        printf("[logic_person_roi_alarm][ch%02d] local event queued: %s\n", ctx->chnId, report.event_id.c_str());
        return;
    }

    log_report_failure_periodically(ctx, state, report);
}

} // namespace

REGISTER_LOGIC(logic_person_roi_alarm);
