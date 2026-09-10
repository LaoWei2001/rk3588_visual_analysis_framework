#include "logic/core/logic_common.h"

#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <memory>
#include <sstream>

namespace
{

constexpr const char *COCO17_KEYPOINT_NAMES[] = {
    "nose",           "left_eye",   "right_eye",   "left_ear",   "right_ear",   "left_shoulder",
    "right_shoulder", "left_elbow", "right_elbow", "left_wrist", "right_wrist", "left_hip",
    "right_hip",      "left_knee",  "right_knee",  "left_ankle", "right_ankle",
};

struct PoseKeypointPrinterState
{
    uint64_t last_print_ms = 0;
};

bool print_due(const PoseKeypointPrinterState &state, uint64_t now_ms, uint64_t interval_ms)
{
    if (interval_ms == 0 || state.last_print_ms == 0 || now_ms < state.last_print_ms)
        return true;
    return now_ms - state.last_print_ms >= interval_ms;
}

void logic_pose_keypoints(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results)
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<PoseKeypointPrinterState>();
    PoseKeypointPrinterState &state = *std::static_pointer_cast<PoseKeypointPrinterState>(*ctx->state);

    const int64_t configured_interval_ms = ctx->param_int("print_interval_ms");
    const uint64_t interval_ms = configured_interval_ms > 0 ? static_cast<uint64_t>(configured_interval_ms) : 0;
    if (!print_due(state, ctx->timestamp_ms, interval_ms))
        return;
    state.last_print_ms = ctx->timestamp_ms;

    const float minimum_keypoint_score = ctx->param_float("min_keypoint_score");
    std::ostringstream output;
    output << std::fixed << std::setprecision(1);

    size_t pose_count = 0;
    for (const AlgoResult &result : *ctx->results)
    {
        if (!result.is_coco17_pose())
            continue;

        ++pose_count;
        output << "[logic_pose_keypoints][ch" << std::setw(2) << std::setfill('0') << ctx->chnId << std::setfill(' ')
               << "][frame=" << ctx->frame_id << "][pose=" << pose_count << "]"
               << " model=" << (result.model_type.empty() ? "pose" : result.model_type) << " track=" << result.track_id
               << " score=" << std::setprecision(3) << result.score << '\n';

        for (size_t index = 0; index < result.keypoints.size(); ++index)
        {
            const cv::Point2f &point = result.keypoints[index];
            const float score = result.keypoint_scores[index];
            output << "  [" << std::setw(2) << index << "] " << std::left << std::setw(14)
                   << COCO17_KEYPOINT_NAMES[index] << std::right << " x=" << std::setw(7) << std::setprecision(1)
                   << point.x << " y=" << std::setw(7) << point.y << " score=" << std::setprecision(3) << score
                   << (score >= minimum_keypoint_score ? " valid" : " low") << '\n';
        }
    }

    if (pose_count == 0)
    {
        output << "[logic_pose_keypoints][ch" << std::setw(2) << std::setfill('0') << ctx->chnId << std::setfill(' ')
               << "][frame=" << ctx->frame_id << "] no COCO17 pose detected\n";
    }

    /* 单次写入整帧文本，尽量避免多通道日志按行交错。 */
    std::fputs(output.str().c_str(), stdout);
    std::fflush(stdout);
}

} // namespace

REGISTER_LOGIC(logic_pose_keypoints);
