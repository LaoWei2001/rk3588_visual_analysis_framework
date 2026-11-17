#include <channel.h>

static void logic_roi_person_count_demo(ChannelContext *ctx)
{
    TargetQuery query;
    query.labels = {"person"};
    query.roi = ROI_ALL;
    const auto targets = ctx->query_targets(query);
    ctx->publish_int("person_count", targets.count);
}

REGISTER_LOGIC(logic_roi_person_count_demo);
