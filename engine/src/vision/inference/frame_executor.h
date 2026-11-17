#pragma once

#include "inference_task.h"
#include "vision/models/model_group.h"
#include <inference.h>
#include <memory>
#include <set>

/** One persistent executor per live worker or standalone image session.
 * No access to application state, video queues or result publication slots.
 */
class FrameExecutor
{
  public:
    FrameExecutor(int channel_id, std::shared_ptr<ModelGroup> group);
    ~FrameExecutor();
    FrameExecutor(const FrameExecutor &) = delete;
    FrameExecutor &operator=(const FrameExecutor &) = delete;
    bool run(const InferenceTask &task, std::vector<AlgoResult> &results, ModelPerfStat &perf, float &lock_wait_ms,
             std::vector<vision::ModelInferenceStatus> *statuses = nullptr);
    bool is_parallel() const;
    size_t model_count() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void filter_inference_results(std::vector<AlgoResult> &results, const ModelGroup &group, float obj_thresh,
                              float nms_thresh, const std::set<int> &allowed_classes);
