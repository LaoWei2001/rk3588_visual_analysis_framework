#include "model_registry.h"
#include "common/logging.h"
#include "vision/inference/npu_core_assignment.h"
#include "yolo/yolo.h"
#include "yolo/yolo26depth.h"
#include "yolo/yolo26pose.h"
#include "yolo/yolopose.h"
#include "yolo/yoloseg.h"
#include "yolo/yolov8det.h"
#include "zip-depth/zip_depth.h"
#include <fstream>
#include <functional>
#include <stdexcept>

namespace
{
using Factory = std::function<std::shared_ptr<ModelBase>(const std::string &, const std::string &, int, float, float)>;
struct Registration
{
    const char *type;
    Factory create;
};
template <class T>
std::shared_ptr<ModelBase> detector(const std::string &path, const std::string &labels, int core, float score,
                                    float nms)
{
    return std::make_shared<T>(path, labels, core, score, nms);
}
template <class T>
std::shared_ptr<ModelBase> depth(const std::string &path, const std::string &, int core, float, float)
{
    return std::make_shared<T>(path, core);
}
std::shared_ptr<ModelBase> obb_detector(const std::string &path, const std::string &labels, int core, float score,
                                        float nms)
{
    return std::make_shared<YoloV8Det>(path, labels, core, score, nms, true);
}
const std::vector<Registration> &registry()
{
    static const std::vector<Registration> entries = {
#define VISION_MODEL(type, kind, labels, factory) {type, factory},
#include "model_catalog.inc"
#undef VISION_MODEL
    };
    return entries;
}
} // namespace

static std::shared_ptr<ModelBase> create_inference_model(const std::string &type, const std::string &path,
                                                         const std::string &labels, int core, float score, float nms)
{
    for (const auto &entry : registry())
        if (entry.type == type)
            return entry.create(path, labels, core, score, nms);
    return nullptr;
}

std::set<int> names_to_class_ids(const std::vector<std::string> &names, const std::string &label_path)
{
    std::set<int> ids;
    if (names.empty() || label_path.empty())
        return ids;

    std::vector<std::string> local_labels;
    std::ifstream ifs(label_path);
    if (!ifs.is_open())
    {
        log_printf_threadsafe("[Inference] Warning: cannot open label_path %s for class filtering\n",
                              label_path.c_str());
        return ids;
    }
    std::string line;
    while (std::getline(ifs, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        if (!line.empty())
            local_labels.push_back(line);
    }

    for (const auto &name : names)
        for (size_t i = 0; i < local_labels.size(); ++i)
            if (local_labels[i] == name)
            {
                ids.insert((int)i);
                break;
            }
    return ids;
}

std::shared_ptr<ModelGroup> create_model_group(const std::vector<ModelBuildSpec> &specs, int channel_id,
                                               int worker_index, int auto_core_pool)
{
    std::vector<ModelGroup::Entry> entries;
    for (const auto &resolved : specs)
    {
        const auto &spec = resolved.config;
        if (!spec.enable || spec.model_path.empty() || spec.model_type.empty())
            continue;
        const int index = static_cast<int>(entries.size());
        const int core = npu::assigned_core_mask(spec.npu_core, channel_id, worker_index, index, auto_core_pool);
        auto model = create_inference_model(spec.model_type, spec.model_path, spec.label_path, core,
                                            resolved.decode_threshold, spec.nms_thresh);
        if (!model)
            throw std::invalid_argument("Unsupported model type: " + spec.model_type);
        ModelGroup::Entry entry;
        entry.id = spec.id.empty() ? "model_" + std::to_string(index) : spec.id;
        entry.type = spec.model_type;
        entry.model = std::move(model);
        entry.model->set_result_source(entry.id, entry.type, index);
        entry.obj_thresh = resolved.decode_threshold;
        entry.nms_thresh = spec.nms_thresh;
        entry.allowed_classes = names_to_class_ids(spec.detect_classes, spec.label_path);
        entries.push_back(std::move(entry));
        log_printf_threadsafe("[Inference] Created model ch%d worker%d: %s requested_mask=0x%x\n", channel_id,
                              worker_index, spec.model_type.c_str(), core);
    }
    return std::make_shared<ModelGroup>(std::move(entries));
}
