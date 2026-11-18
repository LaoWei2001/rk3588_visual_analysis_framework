/** Video scheduling and publication. Frame execution is shared with image sessions. */
#include "application/runtime/app_ctrl.h"
#include "application/runtime/service_watchdog.h"
#include "common/logging.h"
#include "frame_executor.h"
#include "inference_internal.h"

InferenceRuntime g_inference;
FpsTracker g_fps[MAX_CHANNEL_NUM];
InferencePerfCounters g_perf[MAX_CHANNEL_NUM];

/** @brief 返回 chnId 对应的任务队列下标；找不到返回 -1。*/
int get_queue_idx_for_chn(int chnId)
{
    int q_idx = 0;
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        if (!g_inference.model_groups_per_chn[i].empty())
        {
            if (i == chnId)
                return q_idx;
            q_idx++;
        }
    }
    return -1;
}

void *inference_worker_thread(void *arg)
{
    InferenceWorkerArgs *wa = (InferenceWorkerArgs *)arg;
    int chnId = wa->chnId;
    InferenceTaskQueue *tq_ptr = wa->tq;
    std::shared_ptr<ModelGroup> model_group = wa->model_group;
    delete wa;

    FrameExecutor executor(chnId, model_group);
    const size_t model_group_size = executor.model_count();
    if (executor.is_parallel())
        log_printf_threadsafe("[Inference] ch%d parallel multi-model executor started (%zu models)\n", chnId,
                              model_group_size);

    service_watchdog::Progress watchdog_progress("inference", chnId);
    while (g_inference.running && !g_inference.chn_reload_stop[chnId])
    {
        InferenceTask task;
        {
            pthread_mutex_lock(&tq_ptr->mtx);
            while (tq_ptr->q.empty() && g_inference.running && !g_inference.chn_reload_stop[chnId])
                pthread_cond_wait(&tq_ptr->cv, &tq_ptr->mtx);
            if ((!g_inference.running || g_inference.chn_reload_stop[chnId]) && tq_ptr->q.empty())
            {
                pthread_mutex_unlock(&tq_ptr->mtx);
                break;
            }
            task = tq_ptr->q.front();
            tq_ptr->q.pop();
            pthread_mutex_unlock(&tq_ptr->mtx);
        }

        if (!g_inference.running || g_inference.chn_reload_stop[chnId])
            break;

        ModelGroup *model = model_group.get();
        if (!model)
            continue;

        auto work_begin = std::chrono::steady_clock::now();
        service_watchdog::Scope watchdog_scope(watchdog_progress);
        float queue_wait_ms = std::chrono::duration<float, std::milli>(work_begin - task.enqueue_tp).count();

        std::vector<AlgoResult> results;
        ModelPerfStat perf;
        bool ret = false;

        float lock_wait_ms = 0.0f;
        ret = executor.run(task, results, perf, lock_wait_ms);

        if (ret)
        {
            auto filter_begin = std::chrono::steady_clock::now();
            const float obj_thresh = g_inference.obj_thresh[task.chnId].load(std::memory_order_relaxed);
            const float nms_thresh = g_inference.nms_thresh[task.chnId].load(std::memory_order_relaxed);
            std::shared_ptr<const std::set<int>> allowed_classes;
            {
                pthread_mutex_lock(&g_inference.detect_classes_mtx);
                allowed_classes = g_inference.detect_classes[task.chnId];
                pthread_mutex_unlock(&g_inference.detect_classes_mtx);
            }
            static const std::set<int> all_classes;
            std::vector<AlgoResult> filtered = std::move(results);
            filter_inference_results(filtered, *model, obj_thresh, nms_thresh,
                                     allowed_classes ? *allowed_classes : all_classes);
            auto filter_end = std::chrono::steady_clock::now();

            const uint64_t ts_ms = task.frame_steady_ms != 0 ? task.frame_steady_ms : inference_steady_now_ms();
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
                pthread_mutex_lock(&g_inference.channel_results[task.chnId].mtx);
                if (seq > g_inference.channel_results[task.chnId].latest_seq)
                {
                    g_inference.channel_results[task.chnId].data = std::move(filtered);
                    g_inference.channel_results[task.chnId].frame = task.frame;
                    g_inference.channel_results[task.chnId].frame_steady_ms = ts_ms;
                    g_inference.channel_results[task.chnId].frame_unix_ms = task.frame_unix_ms;
                    g_inference.channel_results[task.chnId].video_time_ms = task.video_time_ms;
                    g_inference.channel_results[task.chnId].activity = task.activity;
                    g_inference.channel_results[task.chnId].latest_seq = seq;
                    g_inference.channel_results[task.chnId].has_new = 1;
                    wrote_new = 1;
                }
                pthread_mutex_unlock(&g_inference.channel_results[task.chnId].mtx);
            }

            if (wrote_new)
            {
                pthread_mutex_lock(&g_inference.result_ready_mtx[task.chnId]);
                g_inference.result_dispatch_pending[task.chnId] = 1;
                pthread_cond_signal(&g_inference.result_ready_cv[task.chnId]);
                pthread_mutex_unlock(&g_inference.result_ready_mtx[task.chnId]);
            }

            g_fps[task.chnId].tick();

            float filter_nms_ms = std::chrono::duration<float, std::milli>(filter_end - filter_begin).count();
            float total_ms =
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - work_begin).count();
            g_perf[task.chnId].accumulate((uint64_t)(std::max(0.0f, queue_wait_ms) * 1000.0f),
                                          (uint64_t)(std::max(0.0f, lock_wait_ms) * 1000.0f),
                                          (uint64_t)(std::max(0.0f, perf.preprocess_ms) * 1000.0f),
                                          (uint64_t)(std::max(0.0f, perf.infer_ms) * 1000.0f),
                                          (uint64_t)(std::max(0.0f, perf.postprocess_ms) * 1000.0f),
                                          (uint64_t)(std::max(0.0f, filter_nms_ms) * 1000.0f),
                                          (uint64_t)(std::max(0.0f, total_ms) * 1000.0f), wrote_new != 0);
        }
        else
            g_perf[task.chnId].record_failure();

        /* 同一实际窗口中的任务吞吐和成功任务平均耗时。失败也会触发周期日志。 */
        const uint64_t now_ms = inference_steady_now_ms();
        InferencePerfCounters::Snapshot snap{};
        if (g_perf[task.chnId].reset_if_due(now_ms, PERF_LOG_WINDOW_MS, snap) && app_ctrl_get_performance_display())
        {
            const double seconds = static_cast<double>(snap.elapsed_ms) / 1000.0;
            if (snap.samples == 0)
            {
                log_printf_threadsafe("[Perf][ch%02d] window=%.3fs completed=0 published=0 failed=%llu "
                                      "infer_fps=0.00 publish_fps=0.00 avg_success_ms=unavailable\n",
                                      task.chnId, seconds, (unsigned long long)snap.failures);
                continue;
            }
            const double div = static_cast<double>(snap.samples) * 1000.0;
            /* parallel 模式下各阶段为模型工作量之和，worker 是同帧实际墙钟耗时。 */
            log_printf_threadsafe(
                "[Perf][ch%02d] window=%.3fs completed=%llu published=%llu failed=%llu "
                "infer_fps=%.2f publish_fps=%.2f mode=%s models=%zu | avg_success_ms "
                "wait_q=%.2f lock=%.2f pre=%.2f rknn=%.2f post=%.2f filter=%.2f "
                "worker=%.2f queue+worker=%.2f\n",
                task.chnId, seconds, (unsigned long long)snap.samples, (unsigned long long)snap.published,
                (unsigned long long)snap.failures, snap.samples / seconds, snap.published / seconds,
                model_group_size > 1 ? (executor.is_parallel() ? "parallel" : "serial") : "single", model_group_size,
                snap.wait / div, snap.lock / div, snap.pre / div, snap.npu / div, snap.post / div,
                snap.filter_nms / div, snap.total / div, (static_cast<double>(snap.wait) + snap.total) / div);
        }
    }
    return nullptr;
}
