#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "model_base.h"

// 一次任务使用的实际模型集合；集合没有自己的输入尺寸，也不执行推理。
class ModelGroup
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

    explicit ModelGroup(std::vector<Entry> entries);
    ~ModelGroup() = default;

    const std::vector<Entry> &entries() const
    {
        return entries_;
    }

    /* 合并已经完成的子模型结果。succeeded/child_perf/child_results 按 entries() 对齐。
     * 过滤、模型来源标注和排序统一在这里完成，保证并行与串行路径行为一致。 */
    bool merge_child_results(std::vector<std::vector<AlgoResult>> &child_results,
                             const std::vector<unsigned char> &succeeded, const std::vector<ModelPerfStat> &child_perf,
                             std::vector<AlgoResult> &results, ModelPerfStat *perf = nullptr) const;

  private:
    static void nms_inplace(std::vector<AlgoResult> &results, float threshold);

    std::vector<Entry> entries_;
};
