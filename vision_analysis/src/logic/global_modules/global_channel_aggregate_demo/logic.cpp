#include "logic/core/global_logic.h"
#include "inference/inference_engine.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

namespace
{

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
    const int64_t max_age_ms = gctx->param_int("max_data_age_ms");
    const int64_t total_count_threshold = gctx->param_int("total_count_threshold");
    const bool require_local_alarm = gctx->param_bool("require_local_alarm");
    std::string channel_count_log;

    auto collect = [&](const ChannelLogicSnapshot &channel, int) {
        int64_t count = 0;
        bool local_alarm = false;
        double risk_ratio = 0.0;
        const bool count_valid = channel.read_int("target_count", &count, max_age_ms);
        const bool alarm_valid = channel.read_bool("local_alarm", &local_alarm, max_age_ms);
        const bool risk_valid = channel.read_number("risk_ratio", &risk_ratio, max_age_ms);

        if (!channel_count_log.empty())
            channel_count_log += " ";
        channel_count_log += "ch" + std::to_string(channel.channel_id) + "=";
        if (count_valid)
            channel_count_log += std::to_string(count);
        else
            channel_count_log += "N/A";
        channel_count_log += "(age=" + std::to_string(channel.publication_age_ms) + "ms";
        if (!count_valid || !alarm_valid || !risk_valid)
            channel_count_log += ",invalid";
        channel_count_log += ")";

        if (!count_valid || !alarm_valid || !risk_valid)
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

    const bool alarm =
        valid_channels > 0 && total_count >= total_count_threshold && (!require_local_alarm || alarm_channels > 0);

    const char *instance_id =
        (gctx->config && !gctx->config->instance_id.empty()) ? gctx->config->instance_id.c_str() : "unknown";
    std::printf("[GlobalAggregate][%s][tick=%lld] %s | valid=%lld total=%lld "
                "alarm_channels=%lld threshold=%lld require_local_alarm=%d alarm=%d\n",
                instance_id, static_cast<long long>(gctx->tick_id), channel_count_log.c_str(),
                static_cast<long long>(valid_channels), static_cast<long long>(total_count),
                static_cast<long long>(alarm_channels), static_cast<long long>(total_count_threshold),
                require_local_alarm ? 1 : 0, alarm ? 1 : 0);
    std::fflush(stdout);

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

} // namespace

REGISTER_GLOBAL_LOGIC(global_channel_aggregate_demo);
REGISTER_GLOBAL_LOGIC_ACTION(global_channel_aggregate_demo, global_channel_aggregate_demo_action);
