#include "model_group.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "detection_geometry.h"
#include "result_masks.h"

ModelGroup::ModelGroup(std::vector<Entry> entries) : entries_(std::move(entries))
{
    if (entries_.empty())
        throw std::invalid_argument("ModelGroup requires at least one child model");
    for (const auto &entry : entries_)
    {
        if (!entry.model)
            throw std::invalid_argument("ModelGroup child model is null");
        // Each child is prepared and mapped independently by the executor.
        if (entry.model->input_width() <= 0 || entry.model->input_height() <= 0)
            throw std::invalid_argument("ModelGroup child has invalid input dimensions");
    }
}

void ModelGroup::nms_inplace(std::vector<AlgoResult> &results, float threshold)
{
    std::sort(results.begin(), results.end(),
              [](const AlgoResult &a, const AlgoResult &b) { return a.score > b.score; });
    std::vector<AlgoResult> kept;
    std::vector<unsigned char> removed(results.size(), 0);
    for (size_t i = 0; i < results.size(); ++i)
    {
        if (removed[i])
            continue;
        kept.push_back(results[i]);
        for (size_t j = i + 1; j < results.size(); ++j)
        {
            if (!results[i].is_depth() && !results[j].is_depth() && !removed[j] &&
                results[i].class_id == results[j].class_id && compute_iou(results[i].box, results[j].box) > threshold)
                removed[j] = 1;
        }
    }
    results.swap(kept);
}

bool ModelGroup::merge_child_results(std::vector<std::vector<AlgoResult>> &child_results,
                                     const std::vector<unsigned char> &succeeded,
                                     const std::vector<ModelPerfStat> &child_perf, std::vector<AlgoResult> &results,
                                     ModelPerfStat *perf) const
{
    results.clear();
    float preprocess_ms = 0.0f;
    float infer_ms = 0.0f;
    float postprocess_ms = 0.0f;
    bool any_success = false;

    const size_t count =
        std::min(entries_.size(), std::min(child_results.size(), std::min(succeeded.size(), child_perf.size())));
    for (size_t model_index = 0; model_index < count; ++model_index)
    {
        if (!succeeded[model_index])
            continue;
        any_success = true;
        const Entry &entry = entries_[model_index];
        auto &one_model_results = child_results[model_index];
        preprocess_ms += child_perf[model_index].preprocess_ms;
        infer_ms += child_perf[model_index].infer_ms;
        postprocess_ms += child_perf[model_index].postprocess_ms;

        if (entries_.size() > 1)
        {
            ResultMasks masks(one_model_results);
            one_model_results.erase(std::remove_if(one_model_results.begin(), one_model_results.end(),
                                                   [&](const AlgoResult &result) {
                                                       if (result.is_depth())
                                                           return false;
                                                       return !std::isfinite(result.score) ||
                                                              result.score < entry.obj_thresh ||
                                                              (!entry.allowed_classes.empty() &&
                                                               entry.allowed_classes.count(result.class_id) == 0);
                                                   }),
                                    one_model_results.end());
            if (!entry.model->nms_done())
                nms_inplace(one_model_results, entry.nms_thresh);
            const size_t result_limit = entry.type == "yolov8_obb" ? 128 : 20;
            if (one_model_results.size() > result_limit)
                one_model_results.resize(result_limit);

            masks.restore(one_model_results);
        }
        for (auto &result : one_model_results)
        {
            result.model_id = entry.id;
            result.model_type = entry.type;
            result.model_index = static_cast<int>(model_index);
            results.push_back(std::move(result));
        }
    }

    if (entries_.size() > 1)
        std::stable_sort(results.begin(), results.end(),
                         [](const AlgoResult &a, const AlgoResult &b) { return a.score > b.score; });

    if (perf)
    {
        perf->preprocess_ms = preprocess_ms;
        perf->infer_ms = infer_ms;
        perf->postprocess_ms = postprocess_ms;
    }
    return any_success;
}
