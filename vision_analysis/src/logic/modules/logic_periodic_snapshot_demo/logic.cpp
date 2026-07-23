#include "logic/core/logic_common.h"

#include <cstdint>
#include <memory>
#include <string>

namespace
{

struct PeriodicSnapshotDemoState
{
    uint64_t next_report_ms = 0;
    uint64_t created_event_count = 0;
    uint64_t last_failure_log_ms = 0;
};

PeriodicSnapshotDemoState &periodic_snapshot_demo_state(ChannelContext *ctx)
{
    if (!*ctx->state)
        *ctx->state = std::make_shared<PeriodicSnapshotDemoState>();
    return *std::static_pointer_cast<PeriodicSnapshotDemoState>(*ctx->state);
}

uint64_t report_interval_ms(const ChannelContext *ctx)
{
    const int64_t interval_sec = ctx->param_int("report_interval_sec");
    return static_cast<uint64_t>(std::max<int64_t>(1, interval_sec)) * 1000ULL;
}

void draw_number_at_top_right(ChannelContext *ctx, int64_t display_number)
{
    const std::string text = std::to_string(display_number);
    const int frame_width = std::max(1, ctx->frame->cols);

    /* FreeType 数字在 font_scale=1.0 时约 18~20 px 宽。按 20 px 估算并右对齐；
     * 先画黑色粗体阴影，保证亮、暗背景下都能看清。DISPLAY 指令也会被
     * image_overlay=custom 的截图复用，而 image_overlay=none 会完全跳过叠加。 */
    const int estimated_width = std::max(20, static_cast<int>(text.size()) * 20);
    const int x = std::max(8, frame_width - estimated_width - 16);
    const int y = 40;
    draw_text(ctx, text.c_str(), cv::Point(x + 2, y + 2), cv::Scalar(0, 0, 0), 1.0, 4, DrawCommand::DISPLAY);
    draw_text(ctx, text.c_str(), cv::Point(x, y), cv::Scalar(0, 255, 255), 1.0, 2, DrawCommand::DISPLAY);
}

} // namespace

static void logic_periodic_snapshot_demo(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->frame || ctx->frame->empty())
        return;

    PeriodicSnapshotDemoState &state = periodic_snapshot_demo_state(ctx);
    const int64_t display_number = ctx->param_int("display_number");
    const uint64_t interval_ms = report_interval_ms(ctx);
    const uint64_t now_ms = ctx->timestamp_ms;

    /* 必须先声明绘制再创建事件，带叠加截图才会包含这一帧右上角的数字。 */
    draw_number_at_top_right(ctx, display_number);

    /* 首次进入逻辑时先等待一个完整间隔，不在启动瞬间额外上报。 */
    if (state.next_report_ms == 0)
    {
        state.next_report_ms = now_ms + interval_ms;
        return;
    }
    if (now_ms < state.next_report_ms)
        return;

    /* 不追补掉线或卡顿期间错过的截图，避免恢复后瞬间堆积大量事件。 */
    state.next_report_ms = now_ms + interval_ms;

    const uint64_t next_sequence = state.created_event_count + 1;
    AlarmRequest request;
    request.type = "periodic_snapshot_demo";
    request.message = "周期截图上报演示";
    request.merge_enabled = false; /* 每个周期都创建独立截图，不受合并窗口影响。 */
    request.fields.set_number("display_number", static_cast<double>(display_number));
    request.fields.set_number("report_interval_sec", static_cast<double>(interval_ms / 1000ULL));
    request.fields.set_number("report_sequence", static_cast<double>(next_sequence));

    const std::string event_id = alarm_report(ctx, request);
    if (!event_id.empty())
    {
        state.created_event_count = next_sequence;
        state.last_failure_log_ms = 0;
        return;
    }

    /* 没接图片上报节点或事件创建失败时，最多每分钟提示一次，避免短间隔刷屏。 */
    if (state.last_failure_log_ms == 0 || now_ms - state.last_failure_log_ms >= 60000ULL)
    {
        fprintf(stderr,
                "[logic_periodic_snapshot_demo][ch%02d] screenshot event was not created; "
                "configure an enabled image delivery and check the alarm outbox\n",
                ctx->chnId);
        state.last_failure_log_ms = now_ms;
    }
}

REGISTER_LOGIC(logic_periodic_snapshot_demo);
