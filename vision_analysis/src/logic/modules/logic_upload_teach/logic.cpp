#include "logic/core/logic_common.h"

#include <cstdio>
#include <memory>
#include <string>

namespace
{

struct UploadTeachState
{
    bool pending_report = false;
};

UploadTeachState &upload_teach_state(ChannelContext *ctx)
{
    if (!*ctx->state)
        *(ctx->state) = std::make_shared<UploadTeachState>();
    return *std::static_pointer_cast<UploadTeachState>(*ctx->state);
}

ChannelActionResult logic_upload_teach_action(ChannelContext *ctx, const ChannelAction *action)
{
    ChannelActionResult result;
    if (!ctx || !ctx->state || !action)
    {
        result.message = "ctx or action is null";
        return result;
    }
    if (action->name != "trigger_teach_report")
    {
        result.message = "unsupported action: " + action->name;
        return result;
    }

    UploadTeachState &state = upload_teach_state(ctx);
    state.pending_report = true;
    result.handled = true;
    result.message = "报警事件已排队";
    return result;
}

} // namespace

static void logic_upload_teach(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->frame || ctx->frame->empty())
        return;

    UploadTeachState &state = upload_teach_state(ctx);
    if (!state.pending_report)
        return;

    EventRequest request;
    request.event_type = "upload_teach_demo";
    request.message = "Web 按钮触发的报警事件";
    request.merge_mode = EventMergeMode::NEVER;
    const EventReportResult report = report_event(ctx, request);
    if (!report.accepted())
        fprintf(stderr, "[logic_upload_teach][ch%02d] report failed status=%s detail=%s\n", ctx->chnId,
                event_report_status_name(report.status), report.detail.c_str());
    state.pending_report = false;
}

REGISTER_LOGIC(logic_upload_teach);
REGISTER_LOGIC_ACTION(logic_upload_teach, logic_upload_teach_action);
