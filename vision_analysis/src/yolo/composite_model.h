#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "model_base.h"

/* 同一视频帧运行多个不同模型，并把结果合并成一批。
 * 推理引擎可读取 entries() 并行执行子模型；infer() 保留串行安全回退路径。
 * 该类型只存在于推理层，上层逻辑仍只接收 std::vector<AlgoResult>。 */
class CompositeModel : public ModelBase
{
public:
    struct Entry
    {
        std::string id;
        std::string type;
        std::shared_ptr<ModelBase> model;
        float obj_thresh = 0.25f;
        float nms_thresh = 0.45f;
        std::set<int> allowed_classes;
    };

    explicit CompositeModel(std::vector<Entry> entries);
    ~CompositeModel() override = default;

    bool infer(cv::Mat &frame, std::vector<AlgoResult> &results,
               YoloPerfStat *perf = nullptr) override;

    const std::vector<Entry> &entries() const { return entries_; }

    /* 合并已经完成的子模型结果。succeeded/child_perf/child_results 按 entries() 对齐。
     * 过滤、模型来源标注和排序统一在这里完成，保证并行与串行路径行为一致。 */
    bool merge_child_results(std::vector<std::vector<AlgoResult>> &child_results,
                             const std::vector<unsigned char> &succeeded,
                             const std::vector<YoloPerfStat> &child_perf,
                             std::vector<AlgoResult> &results,
                             YoloPerfStat *perf = nullptr) const;
    int input_width() const override { return input_width_; }
    int input_height() const override { return input_height_; }
    void set_thresh(float, float) override {}
    float get_obj_thresh() const override { return 0.0f; }
    bool nms_done() const override { return true; }

private:
    static void nms_inplace(std::vector<AlgoResult> &results, float threshold);

    std::vector<Entry> entries_;
    int input_width_ = 0;
    int input_height_ = 0;
};
