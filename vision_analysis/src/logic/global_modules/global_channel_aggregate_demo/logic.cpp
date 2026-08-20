#include "logic/core/global_logic.h"

#include <algorithm>
#include <memory>

namespace
{

struct AggregateState
{
    bool reported = false;
};

static void global_channel_aggregate_demo(GlobalContext *gctx)
{
    if (!gctx || !gctx->state)
        return;

    if (!*gctx->state)
        *gctx->state = std::make_shared<AggregateState>();
    AggregateState &state = *std::static_pointer_cast<AggregateState>(*gctx->state);

    int64_t total_count = 0;
    int64_t valid_channels = 0;
    int64_t alarm_channels = 0;
    double highest_risk = -1.0;
    int source_channel_id = -1;
    const int64_t max_age_ms = gctx->param_int("max_data_age_ms");

    auto collect = [&](const ChannelLogicSnapshot &channel, int) {
        int64_t count = 0;
        bool local_alarm = false;
        double risk_ratio = 0.0;
        if (!channel.read_int("target_count", &count, max_age_ms) ||
            !channel.read_bool("local_alarm", &local_alarm, max_age_ms) ||
            !channel.read_number("risk_ratio", &risk_ratio, max_age_ms))
            return;

        ++valid_channels;
        total_count += count;
        if (local_alarm)
            ++alarm_channels;
        if (risk_ratio > highest_risk)
        {
            highest_risk = risk_ratio;
            source_channel_id = channel.channel_id;
        }
    };

    /* 有画布连线时聚合连入通道；没有连线时聚合应用全部通道。
     * 其它业务也可以直接使用 gctx->channel(channel_id) 按 ID 选择。 */
    if (gctx->connected_channel_count() > 0)
        gctx->for_each_connected_channel(collect);
    else
        gctx->for_each_channel(collect);

    const bool alarm = valid_channels > 0 && total_count >= gctx->param_int("total_count_threshold") &&
                       (!gctx->param_bool("require_local_alarm") || alarm_channels > 0);
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
        event_field("valid_channel_count", valid_channels),
        event_field("alarm_channel_count", alarm_channels),
        event_field("total_count", total_count),
    };

    const EventReportResult report = report_event(gctx, request);
    if (report.accepted())
        state.reported = true;
}

} // namespace

REGISTER_GLOBAL_LOGIC(global_channel_aggregate_demo);
