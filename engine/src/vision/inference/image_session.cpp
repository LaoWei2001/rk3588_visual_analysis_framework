#include "frame_executor.h"
#include "inference_roi_geometry.h"
#include "inference_roi_validation.h"
#include "npu_core_assignment.h"
#include "npu_device_owner.h"
#include "vision/models/model_group.h"
#include "vision/models/model_registry.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <inference.h>
#include <mutex>
#include <rendering.h>
#include <stdexcept>

namespace vision
{
bool FrameInferenceResult::succeeded() const
{
    return std::any_of(models.begin(), models.end(), [](const ModelInferenceStatus &m) { return m.succeeded; });
}

bool FrameInferenceResult::complete() const
{
    return !models.empty() &&
           std::all_of(models.begin(), models.end(), [](const ModelInferenceStatus &m) { return m.succeeded; });
}

namespace
{
ImageInferenceOptions validated(ImageInferenceOptions options)
{
    if (options.models.empty() || options.models.size() > 8)
        throw std::invalid_argument("Select between 1 and 8 models");
    std::set<std::string> ids;
    size_t enabled = 0;
    for (auto &model : options.models)
    {
        if (model.id.empty() || model.id.size() > 64 || !ids.insert(model.id).second)
            throw std::invalid_argument("Model IDs must be nonempty and unique (maximum 64 characters)");
        if (!model.enable)
            continue;
        ++enabled;
        const auto &types = inference_model_types();
        const auto found = std::find_if(types.begin(), types.end(),
                                        [&](const InferenceModelInfo &type) { return type.type == model.model_type; });
        if (found == types.end())
            throw std::invalid_argument("Unknown model type: " + model.model_type);
        if (!std::isfinite(model.obj_thresh) || model.obj_thresh < 0 || model.obj_thresh > 1 ||
            !std::isfinite(model.nms_thresh) || model.nms_thresh < 0 || model.nms_thresh > 1)
            throw std::invalid_argument("Model thresholds must be finite values between 0 and 1");
        if (model.npu_core < -1 || model.npu_core > 2)
            throw std::invalid_argument("NPU core must be -1, 0, 1 or 2");
        if (!std::ifstream(model.model_path).good())
            throw std::invalid_argument("Model file not found: " + model.model_path);
        if ((found->requires_labels || !model.label_path.empty()) && !std::ifstream(model.label_path).good())
            throw std::invalid_argument("Label file not found: " + model.label_path);
        if (!model.detect_classes.empty() && names_to_class_ids(model.detect_classes, model.label_path).empty())
            throw std::invalid_argument("None of the selected classes exist in the model label file");
    }
    if (!enabled)
        throw std::invalid_argument("Enable at least one model");
    auto &roi = options.inference_roi;
    if (!roi.mode.empty() && !roi.has_roi())
        throw std::invalid_argument("Unsupported inference ROI mode");
    if (roi.resize_mode != "stretch" && roi.resize_mode != "letterbox" && roi.resize_mode != "expand")
        throw std::invalid_argument("Unsupported inference ROI resize mode");
    if (roi.has_roi())
    {
        if (!valid_inference_polygon(roi))
            throw std::invalid_argument("Invalid inference ROI polygon");
        double right = 0, bottom = 0;
        roi.x = roi.y = 1;
        for (const auto &point : roi.polygon)
        {
            roi.x = std::min(roi.x, point.first);
            roi.y = std::min(roi.y, point.second);
            right = std::max(right, point.first);
            bottom = std::max(bottom, point.second);
        }
        roi.width = right - roi.x;
        roi.height = bottom - roi.y;
    }
    return options;
}
} // namespace

struct ImageInferenceSession::Impl
{
    ImageInferenceOptions options;
    npu::DeviceOwner owner; // Declared before models: released after all workers/contexts.
    std::shared_ptr<ModelGroup> models;
    std::unique_ptr<FrameExecutor> executor;
    std::set<int> allowed_classes;
    std::mutex mutex;
    int64_t sequence = 0;

    explicit Impl(const ImageInferenceOptions &requested) : options(validated(requested))
    {
        ChannelConfig channel;
        channel.id = 0;
        channel.models = options.models;
        const std::vector<ChannelConfig> channels{channel};
        const auto placement = npu::channel_auto_placement(channels, 0);
        std::vector<ModelBuildSpec> specs;
        for (const auto &spec : options.models)
            specs.push_back({spec, spec.obj_thresh});
        models = create_model_group(specs, 0, 0, placement.pool);
        if (models->entries().size() == 1)
            allowed_classes = models->entries().front().allowed_classes;
        executor.reset(new FrameExecutor(0, models));
    }
};

ImageInferenceSession::ImageInferenceSession(const ImageInferenceOptions &options) : impl_(new Impl(options))
{
}
ImageInferenceSession::~ImageInferenceSession() = default;

FrameInferenceResult ImageInferenceSession::infer(const cv::Mat &bgr)
{
    if (bgr.empty() || bgr.type() != CV_8UC3)
        throw std::invalid_argument("Input must be a nonempty CV_8UC3 BGR image");
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto begin = std::chrono::steady_clock::now();
    auto pixels = std::make_shared<cv::Mat>(bgr.isContinuous() ? bgr : bgr.clone());
    InferenceTask task{};
    task.chnId = 0;
    task.srcW = bgr.cols;
    task.srcH = bgr.rows;
    task.srcStrH = bgr.cols;
    task.srcStrV = bgr.rows;
    task.srcFmt = RK_FORMAT_BGR_888;
    task.frame_seq = ++impl_->sequence;
    task.enqueue_tp = begin;
    task.frame_steady_ms = std::chrono::duration_cast<std::chrono::milliseconds>(begin.time_since_epoch()).count();
    task.frame = std::make_shared<LazyVideoFrame>(0, nullptr, task.srcW, task.srcH, task.srcStrH, task.srcStrV,
                                                  task.srcFmt, business_coordinates::WIDTH,
                                                  business_coordinates::HEIGHT, pixels->data, pixels);
    task.roi_config = impl_->options.inference_roi;
    if (task.roi_config.roi_only())
        task.roi_mode = InferenceRoiMode::RoiOnly;
    else if (task.roi_config.full_plus_roi())
        task.roi_mode = InferenceRoiMode::FullPlusRoi;
    else if (task.roi_config.full_frame_roi_filter())
        task.roi_mode = InferenceRoiMode::FullFrameRoiFilter;
    const auto &roi = task.roi_config;
    task.source_roi = inference_source_roi(roi, task.srcW, task.srcH);
    FrameInferenceResult output;
    output.source_width = bgr.cols;
    output.source_height = bgr.rows;
    ModelPerfStat perf;
    float wait = 0;
    impl_->executor->run(task, output.results, perf, wait, &output.models);
    const bool single = impl_->models->entries().size() == 1;
    const auto &first = impl_->models->entries().front().model;
    filter_inference_results(output.results, *impl_->models, single ? first->get_obj_thresh() : 0.0f,
                             single ? first->get_nms_thresh() : 1.0f, impl_->allowed_classes);
    for (auto &result : output.results)
    {
        result.chn_id = 0;
        result.frame_id = task.frame_seq;
        result.timestamp_ms = task.frame_steady_ms;
        result.track_id = -1;
        if (!result.boxMask.empty())
            result.boxMask = result.boxMask.clone();
        if (!result.depth_map.empty())
            result.depth_map = result.depth_map.clone();
    }
    output.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    return output;
}

void draw_inference_results(cv::Mat &bgr, const FrameInferenceResult &output, const std::string &model_id)
{
    if (bgr.empty() || bgr.type() != CV_8UC3)
        throw std::invalid_argument("Drawing requires a nonempty CV_8UC3 BGR image");
    std::vector<AlgoResult> selected;
    for (const auto &result : output.results)
        if (model_id.empty() || result.model_id == model_id)
            selected.push_back(result);
    static std::atomic<int64_t> render_version{0};
    vision::OverlayOptions params;
    params.inputW = output.coordinate_width;
    params.inputH = output.coordinate_height;
    params.result_frame_id = ++render_version;
    params.show_fps = false;
    params.show_custom_overlays = false;
    params.results = &selected;
    vision::render_overlays(bgr, params);
}
} // namespace vision
