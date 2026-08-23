#include "inference/inference_engine.h"
#include "logic/core/global_logic.h"

#include <memory>
#include <string>

struct AggregateState
{
    bool reported = false;
};

static AggregateState &aggregate_state(GlobalContext *gctx)
{
    if (!*gctx->state)
        *gctx->state = std::make_shared<AggregateState>();
    return *std::static_pointer_cast<AggregateState>(*gctx->state);
}

static LogicActionResult global_channel_aggregate_demo_action(GlobalContext *gctx, const LogicAction *action)
{
    LogicActionResult result;
    if (!gctx || !gctx->state || !action)
        return result;
    if (action->name != "reset_report")
    {
        result.message = "unknown action";
        return result;
    }

    aggregate_state(gctx).reported = false;
    result.handled = true;
    result.message = "aggregate report rearmed";
    return result;
}

static void global_channel_aggregate_demo(GlobalContext *gctx)
{
    if (!gctx || !gctx->state)
        return;

    AggregateState &state = aggregate_state(gctx);

    int64_t total_count = 0;
    int64_t valid_channels = 0;
    int64_t alarm_channels = 0;
    double highest_risk = -1.0;
    int source_channel_id = -1;
    const int64_t total_count_threshold = gctx->param_int("total_count_threshold");
    const bool require_local_alarm = gctx->param_bool("require_local_alarm");

    for (const ChannelInput &channel : gctx->inputs())
    {
        const int64_t count = channel.get_int("target_count");
        const bool local_alarm = channel.get_bool("local_alarm");
        const double risk_ratio = channel.get_number("risk_ratio");

        ++valid_channels;
        total_count += count;
        if (local_alarm)
            ++alarm_channels;
        if (risk_ratio > highest_risk)
        {
            highest_risk = risk_ratio;
            source_channel_id = channel.channel_id();
        }
    }

    const bool alarm =
        valid_channels > 0 && total_count >= total_count_threshold && (!require_local_alarm || alarm_channels > 0);

    if (!alarm)
    {
        state.reported = false;
        return;
    }
    if (state.reported)
        return;

    EventRequest request;
    request.event_type = "channel_aggregate_alarm";
    request.message = "多通道聚合条件已满足";
    request.source_channel_id = source_channel_id;
    request.fields = {
        event_field("server_event_type", gctx->param_string("server_event_type")),
        event_field("valid_channel_count", valid_channels),
        event_field("alarm_channel_count", alarm_channels),
        event_field("total_count", total_count),
        event_field("invade_flag", gctx->param_int("invade_flag")),
        event_field("yuv_width", inference_get_input_w()),
        event_field("yuv_height", inference_get_input_h()),
        event_field("yuv_flag", gctx->param_string("yuv_flag")),
    };

    const EventReportResult report = report_event(gctx, request);
    if (report.accepted())
        state.reported = true;
}

REGISTER_GLOBAL_LOGIC(global_channel_aggregate_demo);
REGISTER_GLOBAL_LOGIC_ACTION(global_channel_aggregate_demo, global_channel_aggregate_demo_action);
