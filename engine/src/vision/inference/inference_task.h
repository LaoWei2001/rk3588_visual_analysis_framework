#pragma once
#include "common/frame_activity.h"
#include "pipeline/frame_transform.h"
#include <chrono>
#include <config_types.h>

enum class InferenceRoiMode : uint8_t
{
    FullFrame = 0,
    RoiOnly,
    FullPlusRoi,
    FullFrameRoiFilter,
};

struct InferenceTask
{
    int chnId;
    std::shared_ptr<LazyVideoFrame> frame;
    std::chrono::steady_clock::time_point enqueue_tp;
    int64_t frame_seq;
    uint64_t frame_steady_ms;
    uint64_t frame_unix_ms;
    int64_t video_time_ms = -1;
    std::shared_ptr<FrameActivity> activity;
    std::shared_ptr<RgaImportedBuffer> src_buf;
    int srcW, srcH, srcFmt, srcStrH, srcStrV;
    InferenceRoiMode roi_mode = InferenceRoiMode::FullFrame;
    InferenceRoiConfig roi_config;
    cv::Rect source_roi; /* 用户区域的源图包围盒；实际取景由 resize_mode 在 worker 中计算。 */
};
