#include <channel.h>
#include <cstdio>

// Uses only the public SDK, like other project logic modules. No RKNN, RGA,
// model initialization or frame conversion belongs in this callback.
static void logic_depth_demo(ChannelContext *ctx)
{
    if (!ctx) return;
    const AlgoResult *depth = ctx->depth_result(ctx->param_string("depth_model_id"));
    const cv::Point center = ctx->business_center();
    const float meters = depth ? depth->depth_at(center.x, center.y) : 0.0f;
    float nearest = 0.0f;
    if (depth && ctx->results)
        for (const auto &target : *ctx->results)
        {
            if (target.is_depth()) continue;
            const float distance = depth->depth_median(target.box);
            if (distance <= 0.0f) continue;
            if (nearest == 0.0f || distance < nearest) nearest = distance;
            char text[64];
            std::snprintf(text, sizeof(text), "%.2f m", distance);
            draw_text(ctx, text, target.box_center(), cv::Scalar(0,255,255));
        }

    ctx->publish_bool("depth_valid", meters > 0.0f);
    ctx->publish_number("center_depth_m", meters);
    ctx->publish_number("nearest_target_m", nearest);
    ctx->publish_bool("near", meters > 0.0f && meters < ctx->param_float("near_distance_m"));
    char text[128];
    if (meters > 0.0f) std::snprintf(text, sizeof(text), "Center depth: %.2f m", meters);
    else std::snprintf(text, sizeof(text), "Depth unavailable");
    draw_circle(ctx, center, 5, cv::Scalar(0,255,255));
    draw_text(ctx, text, cv::Point(20,40));

    struct State { uint64_t next_log_ms = 0; };
    State *state = ctx->get_state<State>();
    const auto interval = ctx->param_int("log_interval_ms");
    if (state && interval > 0 && ctx->timestamp_ms >= state->next_log_ms)
    {
        std::printf("[DepthDemo] ch=%d frame=%lld center=%.3f m nearest_target=%.3f m valid=%d\n",
                    ctx->chnId, static_cast<long long>(ctx->frame_id), meters, nearest, meters > 0.0f);
        state->next_log_ms = ctx->timestamp_ms + static_cast<uint64_t>(interval);
    }
}

REGISTER_LOGIC(logic_depth_demo);
