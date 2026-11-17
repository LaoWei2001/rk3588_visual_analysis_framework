#include "frame_executor.h"
#include "common/logging.h"
#include "inference_result_mapping.h"
#include "inference_roi_filter.h"
#include "inference_roi_geometry.h"
#include "npu_core_gate.h"
#include "vision/models/detection_geometry.h"
#include "vision/models/model_group.h"
#include "vision/models/result_masks.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <iterator>
#include <thread>

// ROI merging must compare the transformed quadrilaterals, not their envelopes.
static float result_iou(const AlgoResult &a, const AlgoResult &b)
{
    if (!a.has_obb || !b.has_obb)
        return compute_iou(a.box, b.box);
    std::vector<cv::Point2f> pa(a.obb_corners.begin(), a.obb_corners.end());
    std::vector<cv::Point2f> pb(b.obb_corners.begin(), b.obb_corners.end()), intersection;
    const float area = cv::intersectConvexConvex(pa, pb, intersection);
    const double total = std::abs(cv::contourArea(pa)) + std::abs(cv::contourArea(pb)) - area;
    return total > 0 ? area / total : 0;
}

/** @brief 跳过已被 NMS 抑制的候选框，就地过滤。*/
static void nms_inplace(std::vector<AlgoResult> &dets, float nms_thresh)
{
    std::sort(dets.begin(), dets.end(), [](const AlgoResult &a, const AlgoResult &b) { return a.score > b.score; });
    std::vector<AlgoResult> out;
    std::vector<char> removed(dets.size(), 0);
    for (size_t i = 0; i < dets.size(); ++i)
    {
        if (removed[i])
            continue;
        out.push_back(dets[i]);
        for (size_t j = i + 1; j < dets.size(); ++j)
        {
            if (removed[j])
                continue;
            if (dets[i].is_depth() || dets[j].is_depth() || dets[i].class_id != dets[j].class_id)
                continue;
            if (result_iou(dets[i], dets[j]) > nms_thresh)
                removed[j] = 1;
        }
    }
    dets.swap(out);
}

static cv::Mat ensure_cpu_frame(const InferenceTask &task, int model_width, int model_height, bool use_roi,
                                const InferenceRoiTransform &roi_transform, bool mask_polygon, cv::Mat &input_cache)
{
    if (!task.frame || model_width <= 0 || model_height <= 0)
        return {};
    if (use_roi)
    {
        if (!roi_transform.valid())
            return {};
        const cv::Mat *source = task.frame->source_frame();
        if (!source || source->empty())
            return {};
        const auto *polygon = mask_polygon ? &task.roi_config.polygon : nullptr;
        return make_inference_roi_input(*source, roi_transform, model_width, model_height, polygon);
    }

    /* business_frame() 是框架统一业务坐标尺寸，不一定等于当前通道/子模型的输入尺寸。
     * 两者不同时必须从源图生成当前模型实际尺寸，否则 CPU 回退和 RGA 零拷贝会输出
     * 不同坐标域的检测结果。尺寸相同则继续复用惰性缓存。 */
    if (model_width == business_coordinates::WIDTH && model_height == business_coordinates::HEIGHT)
    {
        const cv::Mat *frame = task.frame->business_frame();
        return frame ? *frame : cv::Mat();
    }
    // 大模型回退也直接从原始缓冲缩放，避免先生成完整原图 BGR 再缩放。
    // worker 私有 input_cache 复用跨帧像素内存；零拷贝成功时此路径完全不执行。
    return task.frame->resized_frame(model_width, model_height, input_cache) ? input_cache : cv::Mat();
}

static bool run_model_task(int chnId, const InferenceTask &task, const std::shared_ptr<ModelBase> &model,
                           std::vector<AlgoResult> &results, ModelPerfStat &perf, float &lock_wait_ms,
                           cv::Mat &input_cache, std::string *error = nullptr)
{
    if (error)
        *error = "Model inference failed";
    if (!model)
        return false;

    auto lock_before = std::chrono::steady_clock::now();
    pthread_mutex_lock(&model->infer_mtx);
    auto lock_after = std::chrono::steady_clock::now();
    lock_wait_ms = std::chrono::duration<float, std::milli>(lock_after - lock_before).count();

    bool ret = false;
    try
    {
        /* ROI 几何始终按 YUV420 的偶数像素边界规范化。即使某次 CPU 回退拿到
         * BGR 帧也沿用同一取景框，避免 RGA/CPU 两条路径和 Web 预览相差 1 像素。 */
        const bool align_yuv420 = true;
        const InferenceRoiTransform roi_transform =
            make_inference_roi_transform(task.source_roi, task.srcW, task.srcH, model->input_width(),
                                         model->input_height(), task.roi_config.resize_mode, align_yuv420);
        const InferenceRoiTransform full_transform{cv::Rect(0, 0, task.srcW, task.srcH),
                                                   cv::Rect(0, 0, model->input_width(), model->input_height())};
        auto run_pass = [&](bool use_roi, std::vector<AlgoResult> &pass_results, ModelPerfStat &pass_perf) {
            bool ok = false;
            float input_prepare_ms = 0.0f;
            const int model_fd = model->get_input_fd();
            /* 直接拉伸/补黑边的多边形是真正的多边形输入：外部填黑。
             * expand 的定义是扩展周边上下文，因此保留扩展框内的真实画面。 */
            const bool mask_polygon =
                use_roi && !task.roi_config.is_axis_aligned_rectangle() && task.roi_config.resize_mode != "expand";
            if (task.src_buf && task.src_buf->handle != 0 && model_fd >= 0 && !mask_polygon &&
                (!use_roi ||
                 (roi_transform.valid() && !roi_transform.needs_padding(model->input_width(), model->input_height()))))
            {
                const auto pre_begin = std::chrono::steady_clock::now();
                const int cached_handle = model->get_input_rga_handle();
                const bool rga_ok =
                    use_roi ? rga_crop_resize_handle(chnId, *task.src_buf, roi_transform.source_rect, model_fd,
                                                     model->input_width(), model->input_height(), model->input_width(),
                                                     model->input_height(), RK_FORMAT_RGB_888, cached_handle)
                            : rga_convert_resize_handle(chnId, *task.src_buf, model_fd, model->input_width(),
                                                        model->input_height(), model->input_width(),
                                                        model->input_height(), RK_FORMAT_RGB_888, cached_handle);
                const auto pre_end = std::chrono::steady_clock::now();
                input_prepare_ms += std::chrono::duration<float, std::milli>(pre_end - pre_begin).count();
                if (rga_ok)
                    ok = model->infer_zero_copy(pass_results, &pass_perf);
            }
            if (!ok)
            {
                /* 只有零拷贝失败才生成 CPU BGR；两种路径都严格使用当前模型的输入尺寸。 */
                const auto cpu_pre_begin = std::chrono::steady_clock::now();
                cv::Mat cpu_frame = ensure_cpu_frame(task, model->input_width(), model->input_height(), use_roi,
                                                     roi_transform, mask_polygon, input_cache);
                input_prepare_ms +=
                    std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - cpu_pre_begin).count();
                pass_perf = ModelPerfStat{};
                if (!cpu_frame.empty())
                    ok = model->infer(cpu_frame, pass_results, &pass_perf);
            }
            pass_perf.preprocess_ms += input_prepare_ms;
            return ok;
        };

        std::vector<AlgoResult> full_results;
        std::vector<AlgoResult> roi_results;
        ModelPerfStat full_perf;
        ModelPerfStat roi_perf;
        const bool run_full = task.roi_mode != InferenceRoiMode::RoiOnly;
        const bool run_roi = task.roi_mode == InferenceRoiMode::RoiOnly ||
                             (task.roi_mode == InferenceRoiMode::FullPlusRoi && model->supports_roi_refinement());
        const bool full_ok = run_full && run_pass(false, full_results, full_perf);
        const bool roi_ok = run_roi && run_pass(true, roi_results, roi_perf);

        const auto mapping_begin = std::chrono::steady_clock::now();
        if (full_ok)
        {
            map_results_to_business_frame(task.srcW, task.srcH, full_transform, model->input_width(),
                                          model->input_height(), full_results);
            model->annotate_results(full_results);
            if (task.roi_mode == InferenceRoiMode::FullFrameRoiFilter)
                filter_results_to_inference_roi(full_results, task.roi_config, business_coordinates::WIDTH,
                                                business_coordinates::HEIGHT);
        }
        if (roi_ok)
        {
            map_results_to_business_frame(task.srcW, task.srcH, roi_transform, model->input_width(),
                                          model->input_height(), roi_results);
            model->annotate_results(roi_results);
            /* 补边区域也可能产生候选框；局部推理批次必须始终按用户区域门控。
             * full_plus_roi 的整帧批次仍完整保留，不改变增强模式语义。 */
            filter_results_to_inference_roi(roi_results, task.roi_config, business_coordinates::WIDTH,
                                            business_coordinates::HEIGHT);
        }

        perf.preprocess_ms = full_perf.preprocess_ms + roi_perf.preprocess_ms;
        perf.infer_ms = full_perf.infer_ms + roi_perf.infer_ms;
        perf.postprocess_ms =
            full_perf.postprocess_ms + roi_perf.postprocess_ms +
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - mapping_begin).count();
        ret = full_ok || roi_ok;
        if (ret)
        {
            cv::Mat merged_mask;
            auto merge_mask = [&](const std::vector<AlgoResult> &source) {
                const auto found = std::find_if(source.begin(), source.end(),
                                                [](const AlgoResult &item) { return !item.boxMask.empty(); });
                if (found == source.end())
                    return;
                if (merged_mask.empty())
                    merged_mask = found->boxMask; // 单次推理仅转移引用，不复制整张掩码。
                else if (merged_mask.size() == found->boxMask.size() && merged_mask.type() == found->boxMask.type())
                {
                    merged_mask = merged_mask.clone(); // 只有真正合并整帧/ROI 两张掩码才创建可写副本。
                    cv::Mat foreground;
                    cv::compare(found->boxMask, 0, foreground, cv::CMP_NE);
                    found->boxMask.copyTo(merged_mask, foreground);
                }
            };
            merge_mask(full_results);
            merge_mask(roi_results); /* 局部高分辨率 mask 覆盖重叠的全图 mask。 */

            results.clear();
            results.reserve(full_results.size() + roi_results.size());
            std::move(full_results.begin(), full_results.end(), std::back_inserter(results));
            std::move(roi_results.begin(), roi_results.end(), std::back_inserter(results));
            merge_depth_results(results);
            if (task.roi_mode == InferenceRoiMode::FullPlusRoi)
            {
                if (model->get_nms_thresh() < 1.0f)
                {
                    nms_inplace(results, model->get_nms_thresh());
                }
            }

            if (!merged_mask.empty() && !results.empty())
            {
                for (AlgoResult &result : results)
                    result.boxMask.release();
                results.front().boxMask = std::move(merged_mask);
            }
        }
    }
    catch (const npu::Cancelled &)
    {
        if (error)
            *error = "Inference cancelled";
        ret = false; // shutdown cancels queued work; it is not a driver failure
    }
    catch (const std::exception &e)
    {
        if (error)
            *error = e.what();
        static std::atomic<unsigned int> error_count{0};
        const unsigned int count = ++error_count;
        if (count <= 10 || (count % 100) == 0)
            log_printf_threadsafe("[Inference] ch%d model inference exception (%u): %s\n", chnId, count, e.what());
        ret = false;
    }
    catch (...)
    {
        static std::atomic<unsigned int> unknown_error_count{0};
        const unsigned int count = ++unknown_error_count;
        if (count <= 10 || (count % 100) == 0)
            log_printf_threadsafe("[Inference] ch%d unknown model inference exception (%u)\n", chnId, count);
        ret = false;
    }

    pthread_mutex_unlock(&model->infer_mtx);
    if (ret && error)
        error->clear();
    return ret;
}

/*
 * 一个 ModelGroup 对应一个持久并行执行器。子线程只属于推理引擎，
 * 不暴露给 ChannelContext/业务逻辑；每次 run() 必须等同一 frame_seq 的全部
 * 子模型完成后才合并，因此上层继续看到一批严格同帧的 AlgoResult。
 */
class GroupExecutor
{
  public:
    GroupExecutor(int chnId, std::shared_ptr<ModelGroup> group) : chnId_(chnId), group_(std::move(group))
    {
        if (!group_)
            throw std::invalid_argument("FrameExecutor requires a model group");
        const size_t count = group_->entries().size();
        slots_.reserve(count);
        for (size_t i = 0; i < count; ++i)
            slots_.push_back(std::unique_ptr<Slot>(new Slot()));
        child_results_.resize(count);
        child_perf_.resize(count);
        child_lock_wait_ms_.resize(count, 0.0f);
        child_succeeded_.resize(count, 0);
        child_errors_.resize(count);
    }

    ~GroupExecutor()
    {
        shutdown();
    }

    bool start()
    {
        if (!group_ || slots_.size() < 2)
            return false;
        try
        {
            for (size_t i = 0; i < slots_.size(); ++i)
                slots_[i]->thread = std::thread(&GroupExecutor::child_loop, this, i);
            started_ = true;
            return true;
        }
        catch (const std::exception &e)
        {
            log_printf_threadsafe("[Inference] ch%d cannot start parallel model workers: %s\n", chnId_, e.what());
            shutdown();
            return false;
        }
    }

    bool run(const InferenceTask &task, std::vector<AlgoResult> &results, ModelPerfStat &perf, float &lock_wait_ms)
    {
        for (size_t i = 0; i < slots_.size(); ++i)
        {
            child_results_[i].clear(); // 保留上一帧容量，避免热路径重复分配
            child_perf_[i] = ModelPerfStat{};
            child_lock_wait_ms_[i] = 0.0f;
            child_succeeded_[i] = 0;
        }
        // Thread creation can fail under memory pressure. Still prepare each
        // child from the original video with its own dimensions and ROI policy;
        // resizing a composite canvas would lose ZipDepth input detail/semantics.
        if (!started_)
        {
            lock_wait_ms = 0.0f;
            for (size_t i = 0; i < slots_.size(); ++i)
            {
                child_succeeded_[i] =
                    run_model_task(chnId_, task, group_->entries()[i].model, child_results_[i], child_perf_[i],
                                   child_lock_wait_ms_[i], slots_[i]->input_cache, &child_errors_[i]);
                lock_wait_ms += child_lock_wait_ms_[i];
            }
            return group_->merge_child_results(child_results_, child_succeeded_, child_perf_, results, &perf);
        }
        {
            std::lock_guard<std::mutex> lock(done_mtx_);
            remaining_ = slots_.size();
        }

        for (size_t i = 0; i < slots_.size(); ++i)
        {
            Slot &slot = *slots_[i];
            {
                std::lock_guard<std::mutex> lock(slot.mtx);
                slot.task = &task;
                slot.has_job = true;
            }
            slot.cv.notify_one();
        }

        {
            std::unique_lock<std::mutex> lock(done_mtx_);
            done_cv_.wait(lock, [&] { return remaining_ == 0; });
        }

        lock_wait_ms = 0.0f;
        for (float child_wait : child_lock_wait_ms_)
            lock_wait_ms += child_wait;
        return group_->merge_child_results(child_results_, child_succeeded_, child_perf_, results, &perf);
    }

    void statuses(std::vector<vision::ModelInferenceStatus> &out) const
    {
        out.clear();
        for (size_t i = 0; i < slots_.size(); ++i)
        {
            const auto &entry = group_->entries()[i];
            vision::ModelInferenceStatus status;
            status.model_id = entry.id;
            status.model_type = entry.type;
            status.succeeded = child_succeeded_[i] != 0;
            status.error = child_errors_[i];
            status.input_width = entry.model->input_width();
            status.input_height = entry.model->input_height();
            status.timing = {child_perf_[i].preprocess_ms, child_perf_[i].infer_ms, child_perf_[i].postprocess_ms};
            out.push_back(std::move(status));
        }
    }

    bool is_parallel() const
    {
        return started_;
    }

  private:
    struct Slot
    {
        std::mutex mtx;
        std::condition_variable cv;
        bool stop = false;
        bool has_job = false;
        const InferenceTask *task = nullptr;
        std::thread thread;
        cv::Mat input_cache;
    };

    void child_loop(size_t index)
    {
        Slot &slot = *slots_[index];
        cv::Mat &input_cache = slot.input_cache; // 每个子模型独立复用，串行回退同样保留原始分辨率。
        while (true)
        {
            const InferenceTask *task = nullptr;
            {
                std::unique_lock<std::mutex> lock(slot.mtx);
                slot.cv.wait(lock, [&] { return slot.stop || slot.has_job; });
                if (slot.stop)
                    return;
                task = slot.task;
                slot.has_job = false;
            }

            if (task)
            {
                child_succeeded_[index] =
                    run_model_task(chnId_, *task, group_->entries()[index].model, child_results_[index],
                                   child_perf_[index], child_lock_wait_ms_[index], input_cache, &child_errors_[index])
                        ? 1
                        : 0;
            }

            {
                std::lock_guard<std::mutex> lock(done_mtx_);
                if (remaining_ > 0)
                    --remaining_;
                if (remaining_ == 0)
                    done_cv_.notify_one();
            }
        }
    }

    void shutdown()
    {
        for (auto &slot_ptr : slots_)
        {
            Slot &slot = *slot_ptr;
            {
                std::lock_guard<std::mutex> lock(slot.mtx);
                slot.stop = true;
            }
            slot.cv.notify_one();
        }
        for (auto &slot_ptr : slots_)
            if (slot_ptr->thread.joinable())
                slot_ptr->thread.join();
        started_ = false;
    }

    int chnId_ = -1;
    std::shared_ptr<ModelGroup> group_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<std::vector<AlgoResult>> child_results_;
    std::vector<ModelPerfStat> child_perf_;
    std::vector<float> child_lock_wait_ms_;
    std::vector<unsigned char> child_succeeded_;
    std::vector<std::string> child_errors_;
    std::mutex done_mtx_;
    std::condition_variable done_cv_;
    size_t remaining_ = 0;
    bool started_ = false;
};

void filter_inference_results(std::vector<AlgoResult> &results, const ModelGroup &group, float obj_thresh,
                              float nms_thresh, const std::set<int> &allowed_classes)
{
    ResultMasks masks(results);
    results.erase(std::remove_if(results.begin(), results.end(),
                                 [&](const AlgoResult &r) {
                                     return !r.is_depth() &&
                                            (!std::isfinite(r.score) || r.score < obj_thresh ||
                                             (!allowed_classes.empty() && allowed_classes.count(r.class_id) == 0));
                                 }),
                  results.end());
    if (group.entries().size() == 1 && !group.entries().front().model->nms_done())
        nms_inplace(results, nms_thresh);
    size_t limit = 100;
    if (group.entries().size() > 1)
    {
        limit = 0;
        for (const auto &entry : group.entries())
            limit += entry.type == "yolov8_obb" ? 128 : 20;
        limit = std::max(size_t(100), limit);
    }
    else if (!results.empty() && results.front().has_obb)
        limit = 128;
    if (results.size() > limit)
        results.resize(limit);
    masks.restore(results);
}

struct FrameExecutor::Impl
{
    std::shared_ptr<ModelGroup> group;
    GroupExecutor executor;
    Impl(int channel, std::shared_ptr<ModelGroup> models) : group(std::move(models)), executor(channel, group)
    {
        executor.start(); // 单模型直接执行；多模型启动失败时使用同一逐模型执行路径。
    }
};

FrameExecutor::FrameExecutor(int channel, std::shared_ptr<ModelGroup> group)
    : impl_(new Impl(channel, std::move(group)))
{
}
FrameExecutor::~FrameExecutor() = default;
bool FrameExecutor::is_parallel() const
{
    return impl_->executor.is_parallel();
}
size_t FrameExecutor::model_count() const
{
    return impl_->group->entries().size();
}
bool FrameExecutor::run(const InferenceTask &task, std::vector<AlgoResult> &results, ModelPerfStat &perf,
                        float &lock_wait_ms, std::vector<vision::ModelInferenceStatus> *statuses)
{
    results.clear();
    perf = ModelPerfStat{};
    lock_wait_ms = 0;
    const bool ok = impl_->executor.run(task, results, perf, lock_wait_ms);
    if (statuses)
        impl_->executor.statuses(*statuses);
    return ok;
}
