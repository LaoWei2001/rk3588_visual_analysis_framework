#pragma once
#include <types.h>

// Preserve each model's combined segmentation payload when filtering its carrier.
class ResultMasks
{
    struct Payload
    {
        std::string id, type;
        int index;
        cv::Mat mask;
    };
    std::vector<Payload> payloads_;

  public:
    explicit ResultMasks(const std::vector<AlgoResult> &results)
    {
        for (const auto &r : results)
            if (!r.boxMask.empty())
                payloads_.push_back({r.model_id, r.model_type, r.model_index, r.boxMask});
    }
    void restore(std::vector<AlgoResult> &results) const
    {
        for (const auto &p : payloads_)
        {
            AlgoResult *first = nullptr;
            for (auto &r : results)
                if (r.model_id == p.id && r.model_type == p.type && r.model_index == p.index)
                {
                    if (!first)
                        first = &r;
                    r.boxMask.release();
                }
            if (first)
                first->boxMask = p.mask;
        }
    }
};
