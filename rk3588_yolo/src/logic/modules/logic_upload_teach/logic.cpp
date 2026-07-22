#include "logic/core/logic_common.h"

#include <cstdint>
#include <memory>
#include <string>

namespace
{

struct UploadTeachState
{
    bool pending_report = false;
    uint64_t created_event_count = 0;
    std::string status = "click the Web action to create a report event";
};

UploadTeachState &upload_teach_state(ChannelContext *ctx)
{
    if (!*ctx->state)
        *ctx->state = std::make_shared<UploadTeachState>();
    return *std::static_pointer_cast<UploadTeachState>(*ctx->state);
}

void draw_target_row(ChannelContext *ctx, const char *label, int y, const cv::Scalar &color, DrawCommand::Target target)
{
    /* draw_rect/draw_text 不直接修改帧像素，而是把 DrawCommand 追加到
     * ctx->draw_cmds；显示、告警图片和事件视频随后按 target 选择性渲染。 */
    draw_rect(ctx, cv::Rect(16, y - 18, 20, 20), color, -1, 0.85, target);
    draw_text(ctx, label, cv::Point(46, y), color, 0.62, 2, target);
}

void draw_upload_teach_legend(ChannelContext *ctx, const UploadTeachState &state)
{
    draw_target_row(ctx, "DISPLAY: live + custom media", 36, cv::Scalar(0, 255, 255), DrawCommand::DISPLAY);
    draw_target_row(ctx, "IMAGE: custom image only", 72, cv::Scalar(0, 255, 0), DrawCommand::IMAGE);
    draw_target_row(ctx, "VIDEO: custom video only", 108, cv::Scalar(0, 165, 255), DrawCommand::VIDEO);
    draw_target_row(ctx, "UPLOAD: custom image + video", 144, cv::Scalar(255, 0, 255), DrawCommand::UPLOAD);
    draw_target_row(ctx, "ALL: live + custom image + video", 180, cv::Scalar(255, 255, 255), DrawCommand::ALL);

    const std::string count_text = "created events: " + std::to_string(state.created_event_count);
    draw_text(ctx, count_text.c_str(), cv::Point(16, 224), cv::Scalar(160, 255, 160), 0.55, 1, DrawCommand::DISPLAY);
    draw_text(ctx, state.status.c_str(), cv::Point(16, 254), cv::Scalar(200, 200, 200), 0.48, 1, DrawCommand::DISPLAY);
}

ChannelActionResult logic_upload_teach_action(ChannelContext *ctx, const ChannelAction &action)
{
    ChannelActionResult result;
    if (!ctx || !ctx->state)
    {
        result.message = "ctx is null";
        return result;
    }
    if (action.name != "trigger_teach_report")
    {
        result.message = "unsupported action: " + action.name;
        return result;
    }

    UploadTeachState &state = upload_teach_state(ctx);
    state.pending_report = true;
    state.status = "report requested; waiting for this frame";
    result.handled = true;
    result.message = "teaching report queued";
    return result;
}

} // namespace

static void logic_upload_teach(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->frame || ctx->frame->empty())
        return;

    UploadTeachState &state = upload_teach_state(ctx);
    draw_upload_teach_legend(ctx, state);
    if (!state.pending_report)
        return;

    const uint64_t sequence = state.created_event_count + 1;

    /* 所有 draw_* 必须放在 alarm_report() 之前：图片任务会在调用期间复制
     * 当前帧 ctx->draw_cmds。Web 的 deliveries 决定同一事件最终发往哪里。 */
    AlarmRequest request;
    request.type = "upload_teach_demo";
    request.message = "统一图片/视频上报教学事件";
    request.merge_enabled = false; /* 每次点击都创建独立事件，便于对照媒体。 */
    request.fields.set_number("event_sequence", static_cast<double>(sequence));
    request.fields.set_string("trigger_source", "web_action");
    request.fields.set_bool("one_call_many_deliveries", true);
    request.fields.set_json("draw_targets", "[\"DISPLAY\",\"IMAGE\",\"VIDEO\",\"UPLOAD\",\"ALL\"]");

    const std::string event_id = alarm_report(ctx, request);
    if (!event_id.empty())
    {
        state.created_event_count = sequence;
        state.status = "event created; inspect outbox and delivery results";
    }
    else
    {
        state.status = "event not created; connect an enabled report node";
    }
    state.pending_report = false;
}

REGISTER_LOGIC(logic_upload_teach);
REGISTER_LOGIC_ACTION(logic_upload_teach, logic_upload_teach_action);
