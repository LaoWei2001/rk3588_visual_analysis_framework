#include "logic/core/logic_common.h"

static void logic_roi_person_count_demo(ChannelContext *ctx)
{
    ctx->publish_int("person_count", roi_count_target(ctx, "person", ROI_ALL));
}

REGISTER_LOGIC(logic_roi_person_count_demo);
