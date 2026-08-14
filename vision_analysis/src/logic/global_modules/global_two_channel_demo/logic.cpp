#include "logic/core/global_logic.h"

#include <algorithm>
#include <memory>

struct TwoChannelDemoState
{
    bool reported = false;
};

static void global_two_channel_demo(GlobalContext *gctx)
{
    if (!gctx || !gctx->state || gctx->channel_count() != 2)
        return;

    if (!*gctx->state)
        *gctx->state = std::make_shared<TwoChannelDemoState>();
    TwoChannelDemoState &state = *std::static_pointer_cast<TwoChannelDemoState>(*gctx->state);

    const ChannelLogicSnapshot *channel_a = gctx->channel_at(0);
    const ChannelLogicSnapshot *channel_b = gctx->channel_at(1);
    if (!channel_a || !channel_b)
        return;

    const int64_t max_age_ms = std::max<int64_t>(1000, gctx->effective_poll_interval_ms * 3);
    if (channel_a->online_state != CH_ONLINE || channel_b->online_state != CH_ONLINE ||
        channel_a->publication_age_ms < 0 || channel_b->publication_age_ms < 0 ||
        channel_a->publication_age_ms > max_age_ms || channel_b->publication_age_ms > max_age_ms)
    {
        state.reported = false;
        return;
    }

    int64_t count_a = 0;
    int64_t count_b = 0;
    bool local_alarm_a = false;
    bool local_alarm_b = false;
    double risk_ratio_a = 0.0;
    double risk_ratio_b = 0.0;
    if (!channel_a->outputs.try_get_int("target_count", &count_a) ||
        !channel_b->outputs.try_get_int("target_count", &count_b) ||
        !channel_a->outputs.try_get_bool("local_alarm", &local_alarm_a) ||
        !channel_b->outputs.try_get_bool("local_alarm", &local_alarm_b) ||
        !channel_a->outputs.try_get_number("risk_ratio", &risk_ratio_a) ||
        !channel_b->outputs.try_get_number("risk_ratio", &risk_ratio_b))
    {
        state.reported = false;
        return;
    }

    const int64_t total_count = count_a + count_b;
    const bool both_local_alarm = local_alarm_a && local_alarm_b;
    const bool alarm = total_count >= gctx->param_int("total_count_threshold") &&
                       (!gctx->param_bool("require_both_local_alarms") || both_local_alarm);

    if (!alarm)
    {
        state.reported = false;
        return;
    }
    if (state.reported)
        return;
    state.reported = true;

    EventRequest request;
    request.event_type = "two_channel_combined_alarm";
    request.message = "双通道组合条件已满足";
    request.source_channel_id = risk_ratio_b > risk_ratio_a ? channel_b->channel_id : channel_a->channel_id;
    request.fields = {
        event_field("channel_a_role", channel_a->parameters.get_string("channel_role")),
        event_field("channel_a_count", count_a),
        event_field("channel_a_threshold", channel_a->parameters.get_int("local_count_threshold")),
        event_field("channel_b_role", channel_b->parameters.get_string("channel_role")),
        event_field("channel_b_count", count_b),
        event_field("channel_b_threshold", channel_b->parameters.get_int("local_count_threshold")),
        event_field("total_count", total_count),
    };
    report_event(gctx, request); /* 与通道 logic 共用同一个上报入口和画布上报节点。 */
}

REGISTER_GLOBAL_LOGIC(global_two_channel_demo);
