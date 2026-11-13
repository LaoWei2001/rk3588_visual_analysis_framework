#include "composite_model.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "detection_geometry.h"
#include "result_masks.h"
#include "vision/models/zip-depth/depth_resize.h"

CompositeModel::CompositeModel(std::vector<Entry> entries) : entries_(std::move(entries))
{
    if (entries_.empty())
        throw std::invalid_argument("CompositeModel requires at least one child model");
    if (!entries_[0].model)
        throw std::invalid_argument("CompositeModel child model is null");
    input_width_ = entries_[0].model->input_width();
    input_height_ = entries_[0].model->input_height();
    for (const auto &entry : entries_)
    {
        if (!entry.model)
            throw std::invalid_argument("CompositeModel child model is null");
        // Each child is prepared and mapped independently by the executor.
        if (entry.model->input_width() <= 0 || entry.model->input_height() <= 0)
            throw std::invalid_argument("CompositeModel child has invalid input dimensions");
    }
}

void CompositeModel::nms_inplace(std::vector<AlgoResult> &results, float threshold)
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

bool CompositeModel::infer(cv::Mat &frame, std::vector<AlgoResult> &results, ModelPerfStat *perf)
{
    std::vector<std::vector<AlgoResult>> child_results(entries_.size());
    std::vector<ModelPerfStat> child_perf(entries_.size());
    std::vector<unsigned char> succeeded(entries_.size(), 0);

    for (size_t model_index = 0; model_index < entries_.size(); ++model_index)
    {
        Entry &entry = entries_[model_index];
        pthread_mutex_lock(&entry.model->infer_mtx);
        bool ok = false;
        try
        {
            ok = entry.model->infer(frame, child_results[model_index], &child_perf[model_index]);
        }
        catch (...)
        {
            // A runtime failure (including the shared NPU gate) must not leave
            // the child locked for future calls or model shutdown.
            pthread_mutex_unlock(&entry.model->infer_mtx);
            throw;
        }
        pthread_mutex_unlock(&entry.model->infer_mtx);
        // Serial CPU fallback receives the composite canvas; detection decoders
        // already return frame coordinates. Depth returns its own model plane.
        if (ok)
            for (auto &result : child_results[model_index])
                if (result.is_depth() && result.depth_map.size() != frame.size())
                {
                    cv::Mat mapped;
                    if (result.is_relative_inverse_depth())
                        zip_depth::resize_depth(result.depth_map, mapped, frame.size());
                    else
                        cv::resize(result.depth_map, mapped, frame.size(), 0, 0, cv::INTER_LINEAR);
                    result.depth_map = std::move(mapped);
                    result.box = cv::Rect(0, 0, frame.cols, frame.rows);
                }
        succeeded[model_index] = ok ? 1 : 0;
    }

    return merge_child_results(child_results, succeeded, child_perf, results, perf);
}

bool CompositeModel::merge_child_results(std::vector<std::vector<AlgoResult>> &child_results,
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

        ResultMasks masks(one_model_results);
        one_model_results.erase(std::remove_if(one_model_results.begin(), one_model_results.end(),
                                               [&](const AlgoResult &result) {
                                                   if (result.is_depth())
                                                       return false;
                                                   return result.score < entry.obj_thresh ||
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
        for (auto &result : one_model_results)
        {
            result.model_id = entry.id;
            result.model_type = entry.type;
            result.model_index = static_cast<int>(model_index);
            results.push_back(std::move(result));
        }
    }

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
