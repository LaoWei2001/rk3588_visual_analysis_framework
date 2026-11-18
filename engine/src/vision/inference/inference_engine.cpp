/**
 * @file inference_engine.cpp
 * @brief 推理引擎公有 API — inference_init / deinit / process_source / take_results …
 *
 * 实现细节说明:
 *   内部数据结构 (InferenceRuntime, InferenceTask, …) 定义在 inference_internal.h。
 *   全局状态 (g_inference, g_fps, g_perf) 和 inference_worker_thread 定义在 inference_executor.cpp。
 *   本文件只持有公有接口层, 不含推理线程逻辑。
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "application/runtime/app_ctrl.h"
#include "common/logging.h"
#include "inference_engine.h"
#include "inference_internal.h"
#include "inference_roi_geometry.h"
#include "model_status.h"
#include "npu_core_assignment.h"
#include "npu_core_gate.h"
#include "npu_device_owner.h"
#include "pipeline/frame_transform.h"
#include "vision/models/model_group.h"
#include "vision/models/model_registry.h"

namespace
{
// Only lifecycle entry/exit accesses this flag; frame processing is unchanged.
bool inference_initialized = false;
// 初始化/销毁由运行时串行调用；热更新在 dispatch_mtx 写锁内访问。
std::unique_ptr<npu::DeviceOwner> inference_device;
std::mutex inference_device_mutex;
bool inference_device_stopping = false;

void acquire_inference_device()
{
    std::lock_guard<std::mutex> lock(inference_device_mutex);
    if (inference_device_stopping)
        throw npu::Cancelled();
    if (!inference_device)
        inference_device.reset(new npu::DeviceOwner());
}

void release_inference_device()
{
    std::lock_guard<std::mutex> lock(inference_device_mutex);
    inference_device.reset();
}

void release_unused_device()
{
    for (const auto &models : g_inference.model_groups_per_chn)
        for (const auto &model : models)
            if (model)
                return;
    release_inference_device();
}

std::vector<ModelBuildSpec> video_model_specs(const ChannelConfig &channel)
{
    std::vector<ModelBuildSpec> specs;
    for (const auto &model : channel.models)
        specs.push_back({model, config_utils::effective_model_obj_thresh(channel, model)});
    return specs;
}

InferenceRoiMode resolve_inference_source_roi(int channel_id, int source_width, int source_height, cv::Rect &out_roi,
                                              InferenceRoiConfig &out_config)
{
    out_roi = cv::Rect(0, 0, source_width, source_height);
    if (source_width <= 0 || source_height <= 0)
        return InferenceRoiMode::FullFrame;
    const auto runtime = app_ctrl_get_runtime_snapshot();
    const ChannelConfig *config = app_ctrl_runtime_channel_config(runtime, channel_id);
    if (!config || !config->inference_roi.has_roi())
        return InferenceRoiMode::FullFrame;

    const InferenceRoiConfig &normalized = config->inference_roi;
    out_config = normalized;
    out_roi = inference_source_roi(normalized, source_width, source_height);
    if (out_roi.empty())
        return InferenceRoiMode::FullFrame;
    if (config->inference_roi.roi_only())
        return InferenceRoiMode::RoiOnly;
    if (config->inference_roi.full_frame_roi_filter())
        return InferenceRoiMode::FullFrameRoiFilter;
    return InferenceRoiMode::FullPlusRoi;
}
} // namespace

/*======================== 取结果（dispatch_worker 调用）========================*/

bool inference_take_results(int chnId, std::vector<AlgoResult> &out, std::shared_ptr<LazyVideoFrame> &out_frame,
                            int64_t &out_frame_id, uint64_t &out_frame_steady_ms, uint64_t &out_frame_unix_ms,
                            int64_t &out_video_time_ms, std::shared_ptr<FrameActivity> &out_activity)
{
    out.clear();
    out_frame.reset();
    out_frame_id = 0;
    out_frame_steady_ms = 0;
    out_frame_unix_ms = 0;
    out_video_time_ms = -1;
    out_activity.reset();
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return false;

    pthread_mutex_lock(&g_inference.channel_results[chnId].mtx);
    if (g_inference.channel_results[chnId].has_new)
    {
        out = std::move(g_inference.channel_results[chnId].data);
        out_frame = std::move(g_inference.channel_results[chnId].frame);
        out_frame_id = g_inference.channel_results[chnId].latest_seq;
        out_frame_steady_ms = g_inference.channel_results[chnId].frame_steady_ms;
        out_frame_unix_ms = g_inference.channel_results[chnId].frame_unix_ms;
        out_video_time_ms = g_inference.channel_results[chnId].video_time_ms;
        out_activity = std::move(g_inference.channel_results[chnId].activity);
        g_inference.channel_results[chnId].has_new = 0;
        pthread_mutex_unlock(&g_inference.channel_results[chnId].mtx);
        return true;
    }
    pthread_mutex_unlock(&g_inference.channel_results[chnId].mtx);
    return false;
}

/*======================== 初始化 ========================*/

int inference_init(const AppConfig &cfg)
{
    {
        std::lock_guard<std::mutex> lock(inference_device_mutex);
        inference_device_stopping = false;
    }
    const int auto_core_pool = npu::preferred_auto_core_pool(cfg.channels);
    log_printf_threadsafe("[NPU] core plan: explicit=0x%x auto_pool=0x%x%s\n",
                          npu::explicitly_assigned_cores(cfg.channels), auto_core_pool,
                          npu::explicitly_assigned_cores(cfg.channels) == 7 ? " (all cores pinned; AUTO would share)"
                                                                            : "");
    model_status_reset(cfg);
    /* 业务坐标始终为 640×640；每个模型独立读取实际输入尺寸。
     * 载入顺序、无模型启动、热切换和反初始化后重启均不能改变业务坐标。 */
    log_printf_threadsafe("[Inference] business canvas %dx%d (independent of model inputs)\n",
                          business_coordinates::WIDTH, business_coordinates::HEIGHT);

    /* 初始化 pthread 同步原语 */
    pthread_rwlock_init(&g_inference.dispatch_mtx, nullptr);
    pthread_mutex_init(&g_inference.detect_classes_mtx, nullptr);
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        pthread_mutex_init(&g_inference.channel_results[i].mtx, nullptr);
        pthread_mutex_init(&g_inference.result_ready_mtx[i], nullptr);
        pthread_cond_init(&g_inference.result_ready_cv[i], nullptr);
    }

    inference_initialized = true;
    struct InitRollback
    {
        bool committed = false;
        ~InitRollback()
        {
            if (!committed)
                inference_deinit();
        }
    } rollback;

    if (std::any_of(cfg.channels.begin(), cfg.channels.end(), [](const ChannelConfig &channel) {
            return channel.enable && config_utils::is_channel_infer_enabled(channel);
        }))
        acquire_inference_device();

    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        g_inference.auto_core_plan[i].store(0);
        g_inference.obj_thresh[i].store(0.0f, std::memory_order_relaxed);
        g_inference.nms_thresh[i].store(1.0f, std::memory_order_relaxed);
    }

    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        g_inference.detect_classes[i] = std::make_shared<const std::set<int>>();

    for (size_t i = 0; i < cfg.channels.size() && i < MAX_CHANNEL_NUM; ++i)
    {
        const auto &channel = cfg.channels[i];
        const int channel_id = channel.id;
        if (channel_id < 0 || channel_id >= MAX_CHANNEL_NUM)
            continue;
        const size_t active_models =
            std::count_if(channel.models.begin(), channel.models.end(), [](const ChannelModelConfig &model) {
                return model.enable && !model.model_path.empty() && !model.model_type.empty();
            });
        /* ModelGroup 已按每个子模型独立过滤，外层过滤必须关闭，避免不同标签表
         * 的 class_id=0 被通道级过滤误删。 */
        if (active_models > 1)
        {
            g_inference.obj_thresh[channel_id] = 0.0f;
            g_inference.nms_thresh[channel_id] = 1.0f;
            g_inference.detect_classes[channel_id] = std::make_shared<const std::set<int>>();
        }
        else
        {
            const ChannelModelConfig *single = nullptr;
            for (const auto &model : channel.models)
                if (model.enable && !model.model_path.empty() && !model.model_type.empty())
                {
                    single = &model;
                    break;
                }
            if (!single)
                continue;
            g_inference.obj_thresh[channel_id] = config_utils::effective_model_obj_thresh(channel, *single);
            g_inference.nms_thresh[channel_id] = single->nms_thresh;
            const std::vector<std::string> &classes = single->detect_classes;
            const std::string &labels = single->label_path;
            g_inference.detect_classes[channel_id] =
                std::make_shared<const std::set<int>>(names_to_class_ids(classes, labels));
        }
    }

    int loaded_models_count = 0;

    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        g_inference.result_dispatch_pending[i] = 0;

    for (size_t i = 0; i < cfg.channels.size() && i < MAX_CHANNEL_NUM; ++i)
    {
        const auto &chn_cfg = cfg.channels[i];
        const int channel_id = chn_cfg.id;
        if (channel_id < 0 || channel_id >= MAX_CHANNEL_NUM)
            continue;
        if (!chn_cfg.enable)
            continue;
        if (!config_utils::is_channel_infer_enabled(chn_cfg))
        {
            log_printf_threadsafe("[Inference] ch%d inference disabled, skip model init\n", channel_id);
            continue;
        }

        try
        {
            model_status_begin(channel_id);
            const auto placement = npu::channel_auto_placement(cfg.channels, channel_id);
            log_printf_threadsafe("[NPU] ch%d automatic pool=0x%x\n", channel_id, placement.pool);
            int threads_for_chn = chn_cfg.threads > 0 ? chn_cfg.threads : 1;
            g_inference.model_groups_per_chn[channel_id].clear();
            const auto specs = video_model_specs(chn_cfg);
            for (int t = 0; t < threads_for_chn; ++t)
            {
                g_inference.model_groups_per_chn[channel_id].push_back(
                    create_model_group(specs, channel_id, t, placement.pool));
                ++loaded_models_count;
            }
            g_inference.auto_core_plan[channel_id].store(placement.key());
        }
        catch (const std::exception &e)
        {
            log_printf_threadsafe("[Inference] Load error ch%d: %s\n", channel_id, e.what());
            loaded_models_count -= g_inference.model_groups_per_chn[channel_id].size();
            g_inference.model_groups_per_chn[channel_id].clear();
            model_status_failed(channel_id, e.what(), true);
        }
    }

    release_unused_device();
    if (loaded_models_count == 0)
        log_printf_threadsafe("[Inference] No active logic models available\n");

    if (!g_inference.running.load())
    {
        g_inference.running.store(true);
        g_inference.max_queue_size = std::max(1, cfg.queue_size);
        g_inference.task_queues.clear();
        // No vector allocation after initializing a queue or creating a worker:
        // every successfully started pthread must retain its join handle.
        g_inference.task_queues.reserve(MAX_CHANNEL_NUM);
        size_t worker_count = 0;
        for (const auto &models : g_inference.model_groups_per_chn)
            worker_count += models.size();
        g_inference.worker_tids.reserve(worker_count);
        g_inference.worker_started.reserve(worker_count);
        int q_count = 0;
        for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        {
            if (!g_inference.model_groups_per_chn[i].empty())
            {
                auto tq = std::make_unique<InferenceTaskQueue>();
                pthread_mutex_init(&tq->mtx, nullptr);
                pthread_cond_init(&tq->cv, nullptr);
                g_inference.task_queues.push_back(std::move(tq));
                q_count++;
            }
        }
        uint64_t now_ms = inference_steady_now_ms();
        for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        {
            g_fps[i].init();
            g_perf[i].init(now_ms);
        }

        /* 创建 worker 线程 */
        int q_idx = 0;
        for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        {
            if (!g_inference.model_groups_per_chn[i].empty())
            {
                InferenceTaskQueue *tq = g_inference.task_queues[q_idx].get();
                bool all_started = true;
                int started_count = 0;
                for (const auto &model : g_inference.model_groups_per_chn[i])
                {
                    InferenceWorkerArgs *wa = new InferenceWorkerArgs{i, tq, model};
                    pthread_t tid{};
                    const int create_rc = pthread_create(&tid, nullptr, inference_worker_thread, wa);
                    g_inference.worker_tids.push_back(tid);
                    g_inference.worker_started.push_back(create_rc == 0 ? 1 : 0);
                    if (create_rc == 0)
                    {
                        ++started_count;
                        log_printf_threadsafe("[Inference] infer_worker created: ch%d tid=%lu\n", i,
                                              (unsigned long)tid);
                    }
                    else
                    {
                        all_started = false;
                        delete wa;
                        log_printf_threadsafe("[Inference] infer_worker create failed: ch%d rc=%d\n", i, create_rc);
                    }
                }
                for (const auto &channel : cfg.channels)
                    if (channel.id == i && started_count > 0)
                        model_status_applied(i, channel, g_inference.model_groups_per_chn[i]);
                if (!all_started)
                    model_status_failed(i, "推理线程创建失败", started_count == 0);
                g_inference.channel_results[i].latest_seq = 0;
                q_idx++;
            }
        }
    }
    rollback.committed = true;
    return 0;
}

/*======================== 反初始化 ========================*/

void inference_request_stop()
{
    if (!inference_initialized)
        return;
    g_inference.running.store(false);
    {
        std::lock_guard<std::mutex> lock(inference_device_mutex);
        inference_device_stopping = true;
        if (inference_device)
            npu::CoreLease::stop();
    }
    /* 必须和等待方使用同一把 mutex，避免“检查 running 后、进入 wait 前”
     * 丢失 broadcast，导致 shutdown 永久卡在 join。 */
    for (auto &tq : g_inference.task_queues)
    {
        pthread_mutex_lock(&tq->mtx);
        pthread_cond_broadcast(&tq->cv);
        pthread_mutex_unlock(&tq->mtx);
    }
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        pthread_mutex_lock(&g_inference.result_ready_mtx[i]);
        pthread_cond_broadcast(&g_inference.result_ready_cv[i]);
        pthread_mutex_unlock(&g_inference.result_ready_mtx[i]);
    }
}

void inference_deinit()
{
    if (!inference_initialized)
        return;
    inference_request_stop();
    model_status_stopped();

    for (size_t i = 0; i < g_inference.worker_tids.size(); ++i)
        if (i < g_inference.worker_started.size() && g_inference.worker_started[i])
            pthread_join(g_inference.worker_tids[i], nullptr);
    g_inference.worker_tids.clear();
    g_inference.worker_started.clear();

    /* 排干所有正在临界区内的 inference_process_source 调用者 */
    pthread_rwlock_wrlock(&g_inference.dispatch_mtx);

    for (auto &tq : g_inference.task_queues)
    {
        pthread_mutex_destroy(&tq->mtx);
        pthread_cond_destroy(&tq->cv);
    }
    g_inference.task_queues.clear();
    for (auto &models : g_inference.model_groups_per_chn)
        models.clear();
    // 所有 worker 已 join，模型已销毁，之后才允许其他进程取得 NPU。
    release_inference_device();

    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        g_inference.channel_results[i].data.clear();
        g_inference.channel_results[i].frame.reset();
        g_inference.channel_results[i].activity.reset();
        g_inference.channel_results[i].has_new = 0;
        pthread_mutex_destroy(&g_inference.channel_results[i].mtx);
        pthread_mutex_destroy(&g_inference.result_ready_mtx[i]);
        pthread_cond_destroy(&g_inference.result_ready_cv[i]);
    }
    pthread_mutex_destroy(&g_inference.detect_classes_mtx);

    pthread_rwlock_unlock(&g_inference.dispatch_mtx);
    pthread_rwlock_destroy(&g_inference.dispatch_mtx);
    inference_initialized = false;
}

/*======================== 帧入队（pipeline_submit_frame 调用）========================*/

int inference_process_source(int chnId, void *source_data, int fd, int srcW, int srcH, int srcFmt, int srcStrH,
                             int srcStrV, int64_t frame_seq, uint64_t frame_steady_ms, uint64_t frame_unix_ms,
                             int64_t video_time_ms, std::shared_ptr<void> source_owner,
                             std::shared_ptr<FrameActivity> activity)
{
    if (!g_inference.running)
        return -1;
    if (!source_data && fd < 0)
        return -1;

    pthread_rwlock_rdlock(&g_inference.dispatch_mtx);
    if (!g_inference.running)
    {
        pthread_rwlock_unlock(&g_inference.dispatch_mtx);
        return -1;
    }
    if (g_inference.task_queues.empty())
    {
        pthread_rwlock_unlock(&g_inference.dispatch_mtx);
        return -1;
    }
    if (chnId >= 0 && chnId < MAX_CHANNEL_NUM && g_inference.chn_reload_stop[chnId])
    {
        pthread_rwlock_unlock(&g_inference.dispatch_mtx);
        return -1;
    }

    int q_idx = 0;
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        if (!g_inference.model_groups_per_chn[i].empty())
        {
            if (i == chnId)
            {
                InferenceTaskQueue &tq = *g_inference.task_queues[q_idx];
                /* FD 只在解码回调期间有效，必须在本函数返回前导入稳定 handle。
                 * 队列中的旧 pending 帧会在入队临界区被这张更新的帧替换。 */
                std::shared_ptr<RgaImportedBuffer> imported;
                if (fd >= 0 && source_owner)
                    imported = rga_import_src_fd(fd, srcW, srcH, srcStrH, srcStrV, srcFmt);
                std::shared_ptr<LazyVideoFrame> lazy_frame = std::make_shared<LazyVideoFrame>(
                    chnId, imported, srcW, srcH, srcStrH, srcStrV, srcFmt, business_coordinates::WIDTH,
                    business_coordinates::HEIGHT, imported ? nullptr : source_data,
                    imported ? source_owner : std::shared_ptr<void>{});
                if (!imported)
                {
                    /* 异步 worker 不能借用解码回调指针。无 DMA-BUF 时只保留原始字节，
                     * BGR 转换和缩放仍由 infer worker 延后执行，避免软件解码源阻塞 appsink。 */
                    size_t source_bytes = 0;
                    if (srcFmt == RK_FORMAT_YCbCr_420_SP || srcFmt == RK_FORMAT_YCrCb_420_SP)
                        source_bytes = static_cast<size_t>(srcStrH) * srcStrV * 3U / 2U;
                    else if (srcFmt == RK_FORMAT_BGR_888 || srcFmt == RK_FORMAT_RGB_888)
                        source_bytes = static_cast<size_t>(srcStrH) * srcStrV * 3U;
                    if (!lazy_frame->retain_borrowed_source(source_bytes))
                    {
                        pthread_rwlock_unlock(&g_inference.dispatch_mtx);
                        return -1;
                    }
                }

                InferenceTask task;
                task.chnId = chnId;
                task.frame = std::move(lazy_frame);
                task.enqueue_tp = std::chrono::steady_clock::now();
                task.frame_seq = frame_seq > 0 ? frame_seq : g_fps[chnId].next_frame_seq();
                task.frame_steady_ms = frame_steady_ms;
                task.frame_unix_ms = frame_unix_ms;
                task.video_time_ms = video_time_ms;
                task.activity = std::move(activity);
                task.src_buf = std::move(imported);
                task.srcW = srcW;
                task.srcH = srcH;
                task.srcFmt = srcFmt;
                task.srcStrH = srcStrH;
                task.srcStrV = srcStrV;
                task.roi_mode = resolve_inference_source_roi(chnId, srcW, srcH, task.source_roi, task.roi_config);
                if (task.roi_mode == InferenceRoiMode::FullFrame)
                    task.source_roi = cv::Rect(0, 0, srcW, srcH);
                std::queue<InferenceTask> superseded;
                pthread_mutex_lock(&tq.mtx);
                const size_t queue_limit = static_cast<size_t>(std::max(1, g_inference.max_queue_size));
                /* 队列达到配置深度时淘汰最旧 pending 帧，为当前最新帧腾位置；
                 * 已经被 worker 领取、正在 NPU 内执行的任务不受影响。queue_size=1
                 * 时即为严格的“最新一帧覆盖旧 pending 帧”。 */
                while (tq.q.size() >= queue_limit)
                {
                    superseded.push(std::move(tq.q.front()));
                    tq.q.pop();
                }
                const size_t discarded_pending = superseded.size();
                tq.q.push(std::move(task));
                pthread_cond_signal(&tq.cv);
                pthread_mutex_unlock(&tq.mtx);
                pthread_rwlock_unlock(&g_inference.dispatch_mtx);
                return 1 + static_cast<int>(discarded_pending);
            }
            q_idx++;
        }
    }
    pthread_rwlock_unlock(&g_inference.dispatch_mtx);
    return 0;
}

/*======================== 查询接口 ========================*/

float inference_get_infer_fps(int chnId)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return 0.0f;
    return g_fps[chnId].value();
}

bool inference_wait_result(int chnId, int timeout_ms)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return false;

    pthread_mutex_lock(&g_inference.result_ready_mtx[chnId]);
    if (!g_inference.result_dispatch_pending[chnId] && g_inference.running)
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L)
        {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g_inference.result_ready_cv[chnId], &g_inference.result_ready_mtx[chnId], &ts);
    }
    int got = g_inference.result_dispatch_pending[chnId] != 0 || !g_inference.running;
    if (got && g_inference.result_dispatch_pending[chnId])
    {
        g_inference.result_dispatch_pending[chnId] = 0;
        pthread_mutex_unlock(&g_inference.result_ready_mtx[chnId]);
        return true;
    }
    pthread_mutex_unlock(&g_inference.result_ready_mtx[chnId]);
    return false;
}

/*======================== 热重载接口 ========================*/

void inference_update_queue_size(int queue_size)
{
    pthread_rwlock_wrlock(&g_inference.dispatch_mtx);
    g_inference.max_queue_size = std::max(1, queue_size);
    pthread_rwlock_unlock(&g_inference.dispatch_mtx);
}

void inference_update_thresh(int chnId, const ChannelConfig &config)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return;
    const auto &models = config.models;
    const size_t active_count = std::count_if(models.begin(), models.end(), [](const ChannelModelConfig &model) {
        return model.enable && !model.model_path.empty() && !model.model_type.empty();
    });
    if (active_count > 1)
    {
        g_inference.obj_thresh[chnId] = 0.0f;
        g_inference.nms_thresh[chnId] = 1.0f;
        return;
    }
    const ChannelModelConfig *single = nullptr;
    for (const auto &model : models)
        if (model.enable && !model.model_path.empty() && !model.model_type.empty())
        {
            single = &model;
            break;
        }
    const float obj_thresh = single ? config_utils::effective_model_obj_thresh(config, *single) : 0.0f;
    const float nms_thresh = single ? single->nms_thresh : 1.0f;
    printf("[AlgoProcess] Updating thresh for channel %d: obj=%.2f, nms=%.2f\n", chnId, obj_thresh, nms_thresh);
    g_inference.obj_thresh[chnId] = obj_thresh;
    g_inference.nms_thresh[chnId] = nms_thresh;

    for (auto &model : g_inference.model_groups_per_chn[chnId])
    {
        if (!model)
            continue;
        // 每通道/worker 都拥有独立实例；不再扫描旧的跨通道共享模型。
        model->entries().front().model->set_thresh(obj_thresh, nms_thresh);
    }
}

void inference_update_detect_classes(int chnId, const ChannelConfig &config)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return;
    const auto &models = config.models;
    const size_t active_count = std::count_if(models.begin(), models.end(), [](const ChannelModelConfig &model) {
        return model.enable && !model.model_path.empty() && !model.model_type.empty();
    });
    if (active_count > 1)
    {
        pthread_mutex_lock(&g_inference.detect_classes_mtx);
        g_inference.detect_classes[chnId] = std::make_shared<const std::set<int>>();
        pthread_mutex_unlock(&g_inference.detect_classes_mtx);
        return;
    }
    const ChannelModelConfig *single = nullptr;
    for (const auto &model : models)
        if (model.enable && !model.model_path.empty() && !model.model_type.empty())
        {
            single = &model;
            break;
        }
    auto ids = std::make_shared<const std::set<int>>(
        single ? names_to_class_ids(single->detect_classes, single->label_path) : std::set<int>());
    pthread_mutex_lock(&g_inference.detect_classes_mtx);
    g_inference.detect_classes[chnId] = std::move(ids);
    pthread_mutex_unlock(&g_inference.detect_classes_mtx);
}

int inference_get_auto_core_plan(int chnId)
{
    return chnId >= 0 && chnId < MAX_CHANNEL_NUM ? g_inference.auto_core_plan[chnId].load() : 0;
}

bool inference_reload_channel_model(int chnId, const ChannelConfig &new_cfg, const npu::AutoCorePlacement &placement)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM || !g_inference.running)
        return false;

    struct DispatchLock
    {
        pthread_rwlock_t *mutex;
        explicit DispatchLock(pthread_rwlock_t *value) : mutex(value)
        {
            pthread_rwlock_wrlock(mutex);
        }
        ~DispatchLock()
        {
            pthread_rwlock_unlock(mutex);
        }
    } dispatch_lock(&g_inference.dispatch_mtx);

    // 必须早于 old_models/new_models 声明：最后一个临时模型引用销毁后才释放设备。
    struct DeviceRelease
    {
        ~DeviceRelease()
        {
            release_unused_device();
        }
    } device_release;

    // Reports every early return, while preserving the previous active model on rollback.
    struct ReloadStatus
    {
        int channel;
        bool applied = false;
        bool clear_active = false;
        std::string error = "模型切换失败，已尝试恢复原模型";
        ~ReloadStatus()
        {
            if (!applied)
                model_status_failed(channel, error, clear_active);
        }
    } status{chnId};
    model_status_begin(chnId);

    /* 找到此通道的任务队列（定义在 inference_executor.cpp，声明在 inference_internal.h）*/
    int q_idx = get_queue_idx_for_chn(chnId);
    if (q_idx < 0 || q_idx >= (int)g_inference.task_queues.size())
    {
        status.error = "通道没有推理队列，需要重启程序启用推理";
        log_printf_threadsafe("[Inference] ch%d has no task queue, skipping model reload\n", chnId);
        return false;
    }
    InferenceTaskQueue *tq = g_inference.task_queues[q_idx].get();

    log_printf_threadsafe("[Inference] Reloading model for ch%d: configured_models=%zu\n", chnId,
                          new_cfg.models.size());

    int worker_start = 0;
    for (int i = 0; i < chnId; ++i)
        worker_start += (int)g_inference.model_groups_per_chn[i].size();
    int worker_count = (int)g_inference.model_groups_per_chn[chnId].size();
    const int requested_worker_count = new_cfg.threads > 0 ? new_cfg.threads : 1;
    if (requested_worker_count != worker_count)
    {
        status.error = "线程数量变更需要重启程序";
        log_printf_threadsafe("[Inference] ch%d worker topology change rejected (%d -> %d); restart required\n", chnId,
                              worker_count, requested_worker_count);
        return false;
    }
    if (config_utils::is_channel_infer_enabled(new_cfg))
    {
        try
        {
            acquire_inference_device();
        }
        catch (const std::exception &error)
        {
            // 取得所有权失败时，旧 worker 和配置完全保留。
            status.error = error.what();
            return false;
        }
    }
    const std::vector<std::shared_ptr<ModelGroup>> old_models = g_inference.model_groups_per_chn[chnId];
    const float old_obj_thresh = g_inference.obj_thresh[chnId].load(std::memory_order_relaxed);
    const float old_nms_thresh = g_inference.nms_thresh[chnId].load(std::memory_order_relaxed);
    std::shared_ptr<const std::set<int>> old_detect_classes;
    pthread_mutex_lock(&g_inference.detect_classes_mtx);
    old_detect_classes = g_inference.detect_classes[chnId];
    pthread_mutex_unlock(&g_inference.detect_classes_mtx);

    g_inference.chn_reload_stop[chnId] = 1;
    pthread_mutex_lock(&tq->mtx);
    pthread_cond_broadcast(&tq->cv);
    pthread_mutex_unlock(&tq->mtx);
    for (int w = worker_start; w < worker_start + worker_count; ++w)
    {
        if (w < static_cast<int>(g_inference.worker_started.size()) && g_inference.worker_started[w])
        {
            pthread_join(g_inference.worker_tids[w], nullptr);
            g_inference.worker_started[w] = 0;
        }
    }

    { /* 清空队列残留任务 */
        pthread_mutex_lock(&tq->mtx);
        while (!tq->q.empty())
            tq->q.pop();
        pthread_mutex_unlock(&tq->mtx);
    }

    auto restart_workers = [&](const std::vector<std::shared_ptr<ModelGroup>> &new_models) -> bool {
        g_inference.model_groups_per_chn[chnId] = new_models;
        {
            pthread_mutex_lock(&g_inference.channel_results[chnId].mtx);
            g_inference.channel_results[chnId].data.clear();
            g_inference.channel_results[chnId].frame.reset();
            g_inference.channel_results[chnId].activity.reset();
            g_inference.channel_results[chnId].has_new = 0;
            pthread_mutex_unlock(&g_inference.channel_results[chnId].mtx);
        }
        g_fps[chnId].reset_rate();
        g_perf[chnId].init(inference_steady_now_ms());
        g_inference.chn_reload_stop[chnId] = 0;
        bool all_started = true;
        for (int w = worker_start; w < worker_start + worker_count; ++w)
        {
            int m_idx = w - worker_start;
            const auto model = m_idx < static_cast<int>(new_models.size()) ? new_models[m_idx] : nullptr;
            if (!model)
            {
                // 关闭推理时只保留槽位，不能用空模型创建 FrameExecutor。
                g_inference.worker_started[w] = 0;
                continue;
            }
            InferenceWorkerArgs *wa = new InferenceWorkerArgs{chnId, tq, model};
            const int create_rc = pthread_create(&g_inference.worker_tids[w], nullptr, inference_worker_thread, wa);
            if (create_rc == 0)
                g_inference.worker_started[w] = 1;
            else
            {
                delete wa;
                g_inference.worker_started[w] = 0;
                all_started = false;
                log_printf_threadsafe("[Inference] reload worker create failed: ch%d rc=%d\n", chnId, create_rc);
            }
        }
        return all_started;
    };

    auto stop_reloaded_workers = [&]() {
        g_inference.chn_reload_stop[chnId] = 1;
        pthread_mutex_lock(&tq->mtx);
        pthread_cond_broadcast(&tq->cv);
        pthread_mutex_unlock(&tq->mtx);
        for (int w = worker_start; w < worker_start + worker_count; ++w)
        {
            if (w < static_cast<int>(g_inference.worker_started.size()) && g_inference.worker_started[w])
            {
                pthread_join(g_inference.worker_tids[w], nullptr);
                g_inference.worker_started[w] = 0;
            }
        }
    };

    auto restore_old_workers = [&]() {
        g_inference.obj_thresh[chnId].store(old_obj_thresh, std::memory_order_relaxed);
        g_inference.nms_thresh[chnId].store(old_nms_thresh, std::memory_order_relaxed);
        pthread_mutex_lock(&g_inference.detect_classes_mtx);
        g_inference.detect_classes[chnId] = old_detect_classes;
        pthread_mutex_unlock(&g_inference.detect_classes_mtx);
        if (!restart_workers(old_models))
        {
            status.clear_active = true;
            status.error += "; 原模型线程恢复失败，需要重启程序";
            log_printf_threadsafe("[Inference] CRITICAL: failed to restore all workers for ch%d\n", chnId);
        }
    };

    if (!config_utils::is_channel_infer_enabled(new_cfg))
    {
        if (!restart_workers(std::vector<std::shared_ptr<ModelGroup>>(worker_count)))
        {
            stop_reloaded_workers();
            restore_old_workers();
            return false;
        }
        log_printf_threadsafe("[Inference] ch%d inference disabled after reload\n", chnId);
        g_inference.auto_core_plan[chnId].store(placement.key());
        model_status_applied(chnId, new_cfg, {});
        status.applied = true;
        return true;
    }

    std::vector<std::shared_ptr<ModelGroup>> new_models;
    std::vector<ChannelModelConfig> active_models;
    for (const auto &model : new_cfg.models)
        if (model.enable && !model.model_path.empty() && !model.model_type.empty())
            active_models.push_back(model);

    try
    {
        const auto specs = video_model_specs(new_cfg);
        for (int t = 0; t < worker_count; ++t)
            new_models.push_back(create_model_group(specs, chnId, t, placement.pool));
    }
    catch (const std::exception &e)
    {
        status.error = e.what();
        log_printf_threadsafe("[Inference] Model load error ch%d: %s\n", chnId, e.what());
        /* 旧 worker 已经停止；新模型加载失败时恢复旧模型，避免该通道永久停止推理。 */
        restore_old_workers();
        return false;
    }

    if (new_models.size() != static_cast<size_t>(worker_count))
    {
        log_printf_threadsafe("[Inference] Model reload incomplete for ch%d (%zu/%d workers); restoring old model\n",
                              chnId, new_models.size(), worker_count);
        restore_old_workers();
        return false;
    }
    else
    {
        const bool multi = active_models.size() > 1;
        g_inference.obj_thresh[chnId] =
            multi ? 0.0f : config_utils::effective_model_obj_thresh(new_cfg, active_models[0]);
        g_inference.nms_thresh[chnId] = multi ? 1.0f : active_models[0].nms_thresh;
        pthread_mutex_lock(&g_inference.detect_classes_mtx);
        if (multi)
            g_inference.detect_classes[chnId] = std::make_shared<const std::set<int>>();
        else
        {
            const std::vector<std::string> &classes = active_models[0].detect_classes;
            const std::string &labels = active_models[0].label_path;
            g_inference.detect_classes[chnId] =
                std::make_shared<const std::set<int>>(names_to_class_ids(classes, labels));
        }
        pthread_mutex_unlock(&g_inference.detect_classes_mtx);
        if (!restart_workers(new_models))
        {
            stop_reloaded_workers();
            restore_old_workers();
            return false;
        }
        log_printf_threadsafe("[Inference] ch%d model reload complete (%zu configured models)\n", chnId,
                              active_models.size());
    }
    g_inference.auto_core_plan[chnId].store(placement.key());
    log_printf_threadsafe("[NPU] ch%d active automatic pool=0x%x\n", chnId, placement.pool);
    model_status_applied(chnId, new_cfg, new_models);
    status.applied = true;
    return true;
}
