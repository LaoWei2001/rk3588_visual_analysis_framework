// SDK 实战示例：指定区域持续有目标时报警；策略属于项目，不放入引擎 SDK。
#include <channel.h>
#include <drawing.h>
#include <events.h>
#include <string>

namespace
{
struct DwellState
{
    bool present = false;
    bool reported = false;
    bool attempted = false;
    uint64_t since_ms = 0;
    uint64_t previous_ms = 0;
    uint64_t last_attempt_ms = 0;
};

void logic_roi_dwell_demo(ChannelContext *ctx)
{
    if (!ctx) return;
    auto *state = ctx->get_state<DwellState>();
    if (!state) return;

    // 默认值、类型、取值范围和参数修改时重置状态，都在 logic.json 声明。
    const std::string label = ctx->param_string("target_label");
    const std::string roi_name = ctx->param_string("roi_name");
    const uint64_t hold_ms = static_cast<uint64_t>(ctx->param_int("hold_ms"));
    const uint64_t max_gap_ms = static_cast<uint64_t>(ctx->param_int("max_gap_ms"));
    const uint64_t now = ctx->timestamp_ms;
    const int roi = roi_find(ctx, roi_name.c_str());
    const auto *polygon = ctx->roi_polygon_at(roi);
    const bool roi_valid = polygon && polygon->size() >= 3;
    TargetQuery query;
    query.labels = {label};
    query.roi = roi;
    const auto selected = ctx->query_targets(query);
    const bool valid = selected.valid();
    const int count = selected.count;

    // “持续出现”是区域始终有人，不要求同一个 track_id。
    // 任一帧未检出、推理无效、时间倒退或帧间隔过大，都重新计时。
    if (!valid || count == 0 || (state->present &&
        (now < state->previous_ms || now - state->previous_ms > max_gap_ms)))
        *state = DwellState{};
    if (count > 0 && !state->present)
    {
        state->present = true;
        state->since_ms = now;
    }
    state->previous_ms = now;
    const uint64_t dwell_ms = state->present ? now - state->since_ms : 0;
    const bool alarm = state->present && dwell_ms >= hold_ms;
    const char *status = !roi_valid ? "roi_missing" : !valid ? "inference_unavailable" :
                         count == 0 ? "waiting" : alarm ? "alarm" : "timing";

    // 先画再提交事件，让同帧截图也能包含报警区域和文字。
    const cv::Scalar color = alarm ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 220, 0);
    if (roi_valid)
        draw_polyline(ctx, *polygon, color, 2, 1.0, true);
    const std::string text = std::string(status) + " count=" + std::to_string(count) +
                             " dwell=" + std::to_string(dwell_ms) + "ms";
    draw_rect(ctx, {12, 12, 530, 35}, {0, 0, 0}, -1, 0.75);
    draw_text(ctx, text.c_str(), {20, 35}, color);

    // 一次连续占用只提交一个已接受事件；失败最多每秒重试一次。
    if (alarm && !state->reported &&
        (!state->attempted || now - state->last_attempt_ms >= 1000))
    {
        state->attempted = true;
        state->last_attempt_ms = now;
        EventRequest request;
        request.event_type = "roi_dwell";
        request.message = "区域目标持续出现";
        request.fields = {
            event_field("target_label", label),
            event_field("roi_name", roi_name),
            event_field("target_count", count),
            event_field("dwell_ms", dwell_ms),
        };
        state->reported = report_event(ctx, request).accepted();
    }
    ctx->publish_int("target_count", count);
    ctx->publish_int("dwell_ms", static_cast<int64_t>(dwell_ms));
    ctx->publish_bool("alarm_active", alarm);
    ctx->publish_bool("event_accepted", state->reported);
    ctx->publish_string("status", status);
}
} // namespace

REGISTER_LOGIC(logic_roi_dwell_demo);
