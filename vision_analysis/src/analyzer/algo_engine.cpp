/**
 * @file algo_engine.cpp
 * @brief 推理引擎核心 — 全局状态 + worker 线程 + 私有辅助函数
 *
 * 职责（仅此文件）:
 *   - g_algo / g_fps / g_perf 的唯一定义
 *   - nms_inplace / get_queue_idx_for_chn — 引擎私有辅助（static，不对外）
 *   - names_to_class_ids / create_model   — 跨文件辅助（声明在 algo_internal.h）
 *   - worker_thread_func                  — NPU 推理 worker（声明在 algo_internal.h）
 *
 * 公有 API（algorithm_init / algorithm_process_mat / …）在 algoProcess.cpp。
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../core/app_ctrl.h"
#include "../core/pause_ctrl.h"
#include "../system.h"
#include "../yolo/composite_model.h"
#include "../yolo/yolo.h"
#include "../yolo/yolopose.h"
#include "../yolo/yoloseg.h"
#include "../yolo/yolov8det.h"
#include "algoProcess.h"
#include "algo_internal.h"
#include "frame_pipeline.h"

/* compute_iou 声明在 algoProcess.h / yolo.h 全局命名空间，此处显式引入供 nms_inplace 使用 */
using ::compute_iou;

/*======================== 全局状态定义（唯一定义，其余文件通过 extern 访问）========================*/

AlgoEngine g_algo;
FpsTracker g_fps[MAX_CHANNEL_NUM];
PerfCounters g_perf[MAX_CHANNEL_NUM];

/*======================== 私有辅助（仅本文件可见）========================*/

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
            if (dets[i].class_id != dets[j].class_id)
                continue;
            if (compute_iou(dets[i].box, dets[j].box) > nms_thresh)
                removed[j] = 1;
        }
    }
    dets.swap(out);
}

/** @brief 返回 chnId 对应的任务队列下标；找不到返回 -1。*/
int get_queue_idx_for_chn(int chnId)
{
    int q_idx = 0;
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        if (!g_algo.models_per_chn[i].empty())
        {
            if (i == chnId)
                return q_idx;
            q_idx++;
        }
    }
    return -1;
}

/*======================== 跨文件辅助函数实现 ========================*/

std::set<int> names_to_class_ids(const std::vector<std::string> &names, const std::string &label_path)
{
    std::set<int> ids;
    if (names.empty() || label_path.empty())
        return ids;

    std::vector<std::string> local_labels;
    std::ifstream ifs(label_path);
    if (!ifs.is_open())
    {
        log_printf_threadsafe("[Algo] Warning: cannot open label_path %s for class filtering\n", label_path.c_str());
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

std::shared_ptr<ModelBase> create_model(const std::string &type, const std::string &model_path,
                                        const std::string &label_path, int core_mask, float obj_thresh,
                                        float nms_thresh)
{
    if (type == "yolov8_pose")
        return std::make_shared<YoloPose>(model_path, label_path, core_mask, obj_thresh, nms_thresh);
    if (type == "yolov5_seg")
        return std::make_shared<YoloSeg>(model_path, label_path, core_mask, obj_thresh, nms_thresh);
    if (type == "yolov5")
        return std::make_shared<YOLO>(model_path, label_path, core_mask, obj_thresh, nms_thresh);
    if (type == "yolov8_det")
        return std::make_shared<YoloV8Det>(model_path, label_path, core_mask, obj_thresh, nms_thresh);
    return nullptr;
}

/*======================== NPU 推理 Worker 线程 ========================*/

/**
 * @brief 对一个实际模型执行一帧推理。
 *
 * 单模型 worker 与多模型子线程共用此函数，保证两条路径都优先使用
 * DMA-BUF -> RGA -> RKNN input memory 的零拷贝路径；失败时才回退 CPU Mat。
 */
static bool run_model_task(int chnId, const AlgoTask &task, const std::shared_ptr<ModelBase> &model,
                           std::vector<AlgoResult> &results, YoloPerfStat &perf, float &lock_wait_ms)
{
    if (!model)
        return false;

    auto lock_before = std::chrono::steady_clock::now();
    pthread_mutex_lock(&model->infer_mtx);
    auto lock_after = std::chrono::steady_clock::now();
    lock_wait_ms = std::chrono::duration<float, std::milli>(lock_after - lock_before).count();

    bool ret = false;
    try
    {
        const int model_fd = model->get_input_fd();
        if (task.src_buf && task.src_buf->handle != 0 && model_fd >= 0)
        {
            auto pre_begin = std::chrono::steady_clock::now();
            const int cached_handle = model->get_input_rga_handle();
            const bool rga_ok = rga_convert_resize_handle(chnId, *task.src_buf, model_fd, model->input_width(),
                                                          model->input_height(), model->input_width(),
                                                          model->input_height(), RK_FORMAT_RGB_888, cached_handle);
            auto pre_end = std::chrono::steady_clock::now();
            perf.preprocess_ms = std::chrono::duration<float, std::milli>(pre_end - pre_begin).count();
            if (rga_ok)
                ret = model->infer_zero_copy(results, &perf);
        }

        if (!ret && !task.img.empty())
        {
            /* 每个并行子模型使用独立 Mat 头；像素数据只读共享，避免并发修改同一 Mat 头。 */
            cv::Mat cpu_frame = task.img;
            ret = model->infer(cpu_frame, results, &perf);
        }
    }
    catch (const std::exception &e)
    {
        static std::atomic<unsigned int> error_count{0};
        const unsigned int count = ++error_count;
        if (count <= 10 || (count % 100) == 0)
            log_printf_threadsafe("[Algo] ch%d model inference exception (%u): %s\n", chnId, count, e.what());
        ret = false;
    }
    catch (...)
    {
        static std::atomic<unsigned int> unknown_error_count{0};
        const unsigned int count = ++unknown_error_count;
        if (count <= 10 || (count % 100) == 0)
            log_printf_threadsafe("[Algo] ch%d unknown model inference exception (%u)\n", chnId, count);
        ret = false;
    }

    pthread_mutex_unlock(&model->infer_mtx);
    return ret;
}

/*
 * 一个 CompositeModel 对应一个持久并行执行器。子线程只属于推理引擎，
 * 不暴露给 ChannelContext/业务逻辑；每次 run() 必须等同一 frame_seq 的全部
 * 子模型完成后才合并，因此上层继续看到一批严格同帧的 AlgoResult。
 */
class ParallelCompositeExecutor
{
  public:
    ParallelCompositeExecutor(int chnId, std::shared_ptr<CompositeModel> composite)
        : chnId_(chnId), composite_(std::move(composite))
    {
        const size_t count = composite_ ? composite_->entries().size() : 0;
        slots_.reserve(count);
        for (size_t i = 0; i < count; ++i)
            slots_.push_back(std::unique_ptr<Slot>(new Slot()));
        child_results_.resize(count);
        child_perf_.resize(count);
        child_lock_wait_ms_.resize(count, 0.0f);
        child_succeeded_.resize(count, 0);
    }

    ~ParallelCompositeExecutor()
    {
        shutdown();
    }

    bool start()
    {
        if (!composite_ || slots_.size() < 2)
            return false;
        try
        {
            for (size_t i = 0; i < slots_.size(); ++i)
                slots_[i]->thread = std::thread(&ParallelCompositeExecutor::child_loop, this, i);
            started_ = true;
            return true;
        }
        catch (const std::exception &e)
        {
            log_printf_threadsafe("[Algo] ch%d cannot start parallel model workers: %s\n", chnId_, e.what());
            shutdown();
            return false;
        }
    }

    bool run(const AlgoTask &task, std::vector<AlgoResult> &results, YoloPerfStat &perf, float &lock_wait_ms)
    {
        if (!started_)
            return false;

        for (size_t i = 0; i < slots_.size(); ++i)
        {
            child_results_[i].clear(); // 保留上一帧容量，避免热路径重复分配
            child_perf_[i] = YoloPerfStat{};
            child_lock_wait_ms_[i] = 0.0f;
            child_succeeded_[i] = 0;
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
        return composite_->merge_child_results(child_results_, child_succeeded_, child_perf_, results, &perf);
    }

  private:
    struct Slot
    {
        std::mutex mtx;
        std::condition_variable cv;
        bool stop = false;
        bool has_job = false;
        const AlgoTask *task = nullptr;
        std::thread thread;
    };

    void child_loop(size_t index)
    {
        Slot &slot = *slots_[index];
        while (true)
        {
            const AlgoTask *task = nullptr;
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
                    run_model_task(chnId_, *task, composite_->entries()[index].model, child_results_[index],
                                   child_perf_[index], child_lock_wait_ms_[index])
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
    std::shared_ptr<CompositeModel> composite_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::vector<std::vector<AlgoResult>> child_results_;
    std::vector<YoloPerfStat> child_perf_;
    std::vector<float> child_lock_wait_ms_;
    std::vector<unsigned char> child_succeeded_;
    std::mutex done_mtx_;
    std::condition_variable done_cv_;
    size_t remaining_ = 0;
    bool started_ = false;
};

void *worker_thread_func(void *arg)
{
    WorkerArg *wa = (WorkerArg *)arg;
    int chnId = wa->chnId;
    TaskQueue *tq_ptr = wa->tq;
    std::shared_ptr<ModelBase> model_ptr = wa->model;
    delete wa;

    std::unique_ptr<ParallelCompositeExecutor> parallel_executor;
    size_t model_group_size = 1;
    if (auto composite = std::dynamic_pointer_cast<CompositeModel>(model_ptr))
    {
        model_group_size = composite->entries().size();
        parallel_executor.reset(new ParallelCompositeExecutor(chnId, composite));
        if (parallel_executor->start())
            log_printf_threadsafe("[Algo] ch%d parallel multi-model executor started (%zu models)\n", chnId,
                                  composite->entries().size());
        else
            parallel_executor.reset();
    }

    while (g_algo.running && !g_algo.chn_reload_stop[chnId])
    {
        AlgoTask task;
        {
            pthread_mutex_lock(&tq_ptr->mtx);
            while (tq_ptr->q.empty() && g_algo.running && !g_algo.chn_reload_stop[chnId])
                pthread_cond_wait(&tq_ptr->cv, &tq_ptr->mtx);
            if ((!g_algo.running || g_algo.chn_reload_stop[chnId]) && tq_ptr->q.empty())
            {
                pthread_mutex_unlock(&tq_ptr->mtx);
                break;
            }
            task = tq_ptr->q.front();
            tq_ptr->q.pop();
            pthread_mutex_unlock(&tq_ptr->mtx);
        }

        if (pause_ctrl::is_paused())
            continue;
        if (!g_algo.running || g_algo.chn_reload_stop[chnId])
            break;

        ModelBase *model = model_ptr.get();
        if (!model)
            continue;

        auto work_begin = std::chrono::steady_clock::now();
        float queue_wait_ms = std::chrono::duration<float, std::milli>(work_begin - task.enqueue_tp).count();

        std::vector<AlgoResult> results;
        YoloPerfStat perf;
        bool ret = false;

        float lock_wait_ms = 0.0f;
        if (parallel_executor)
            ret = parallel_executor->run(task, results, perf, lock_wait_ms);
        else
            ret = run_model_task(task.chnId, task, model_ptr, results, perf, lock_wait_ms);

        if (ret)
        {
            auto filter_begin = std::chrono::steady_clock::now();
            float obj_thresh_v = g_algo.obj_thresh[task.chnId].load(std::memory_order_relaxed);
            float nms_thresh_v = g_algo.nms_thresh[task.chnId].load(std::memory_order_relaxed);

            std::vector<AlgoResult> filtered;
            if (obj_thresh_v > model->get_obj_thresh())
            {
                filtered.reserve(results.size());
                for (const auto &d : results)
                    if (d.score >= obj_thresh_v)
                        filtered.push_back(d);
            }
            else
                filtered = std::move(results);

            std::shared_ptr<const std::set<int>> allowed_classes;
            {
                pthread_mutex_lock(&g_algo.detect_classes_mtx);
                allowed_classes = g_algo.detect_classes[task.chnId];
                pthread_mutex_unlock(&g_algo.detect_classes_mtx);
            }
            if (allowed_classes && !allowed_classes->empty())
            {
                std::vector<AlgoResult> class_filtered;
                class_filtered.reserve(filtered.size());
                for (const auto &d : filtered)
                    if (allowed_classes->count(d.class_id))
                        class_filtered.push_back(d);
                filtered.swap(class_filtered);
            }

            if (!model->nms_done())
                nms_inplace(filtered, nms_thresh_v);

            /* 多模型通道的每个子模型最多保留20个结果；合并后不能仍截断为20，
             * 否则排在后面的姿态模型结果可能被第一个检测模型完全挤掉。 */
            const size_t MAX_DET_PER_FRAME = 100;
            if (filtered.size() > MAX_DET_PER_FRAME)
                filtered.resize(MAX_DET_PER_FRAME);
            auto filter_end = std::chrono::steady_clock::now();

            uint64_t ts_ms = algo_steady_now_ms();
            int64_t seq = task.frame_seq;
            for (auto &r : filtered)
            {
                r.track_id = -1;
                r.chn_id = task.chnId;
                r.frame_id = seq;
                r.timestamp_ms = ts_ms;
            }

            int wrote_new = 0;
            {
                pthread_mutex_lock(&g_algo.channel_results[task.chnId].mtx);
                if (seq > g_algo.channel_results[task.chnId].latest_seq)
                {
                    g_algo.channel_results[task.chnId].data = std::move(filtered);
                    g_algo.channel_results[task.chnId].data_frame = task.img;
                    g_algo.channel_results[task.chnId].latest_seq = seq;
                    g_algo.channel_results[task.chnId].has_new = 1;
                    wrote_new = 1;
                }
                pthread_mutex_unlock(&g_algo.channel_results[task.chnId].mtx);
            }

            if (wrote_new)
            {
                pthread_mutex_lock(&g_algo.result_ready_mtx[task.chnId]);
                g_algo.result_dispatch_pending[task.chnId] = 1;
                pthread_cond_signal(&g_algo.result_ready_cv[task.chnId]);
                pthread_mutex_unlock(&g_algo.result_ready_mtx[task.chnId]);
            }

            g_fps[task.chnId].tick();

            float filter_nms_ms = std::chrono::duration<float, std::milli>(filter_end - filter_begin).count();
            float total_ms = std::chrono::duration<float, std::milli>(filter_end - work_begin).count();
            g_perf[task.chnId].accumulate(
                (uint64_t)(std::max(0.0f, queue_wait_ms) * 1000.0f), (uint64_t)(std::max(0.0f, lock_wait_ms) * 1000.0f),
                (uint64_t)(std::max(0.0f, perf.preprocess_ms) * 1000.0f),
                (uint64_t)(std::max(0.0f, perf.infer_ms) * 1000.0f),
                (uint64_t)(std::max(0.0f, perf.postprocess_ms) * 1000.0f),
                (uint64_t)(std::max(0.0f, filter_nms_ms) * 1000.0f), (uint64_t)(std::max(0.0f, total_ms) * 1000.0f));

            /* 周期性 Perf 日志 */
            uint64_t now_ms = algo_steady_now_ms();
            PerfCounters::Snapshot snap{};
            if (g_perf[task.chnId].reset_if_due(now_ms, PERF_LOG_WINDOW_MS, snap))
            {
                if (app_ctrl_get_performance_display())
                {
                    float div = (float)snap.samples * 1000.0f;
                    /* parallel 模式下 pre/npu/post 是各模型工作量之和；total 是同帧并行后的真实墙钟耗时。 */
                    log_printf_threadsafe("[Perf][ch%02d][5s] fps=%.1f mode=%s models=%zu | "
                                          "wait_q=%.2f lock=%.2f pre=%.2f npu=%.2f post=%.2f nms=%.2f | total=%.2fms\n",
                                          task.chnId, g_fps[task.chnId].value(),
                                          parallel_executor ? "parallel" : "single", model_group_size,
                                          (float)snap.wait / div, (float)snap.lock / div, (float)snap.pre / div,
                                          (float)snap.npu / div, (float)snap.post / div, (float)snap.filter_nms / div,
                                          (float)snap.total / div);
                }
            }
        }
    }
    return nullptr;
}
