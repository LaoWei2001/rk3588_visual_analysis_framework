/**
 * @file algoProcess.cpp
 * @brief 推理引擎公有 API — algorithm_init / deinit / process_mat /
 * take_results …
 *
 * 实现细节说明:
 *   内部数据结构 (AlgoEngine, AlgoTask, …) 定义在 algo_internal.h。
 *   全局状态 (g_algo, g_fps, g_perf) 和 worker_thread_func 定义在
 * algo_engine.cpp。 本文件只持有公有接口层, 不含推理线程逻辑。
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <set>
#include <string>
#include <vector>

#include "../core/app_ctrl.h"
#include "../system.h"
#include "../yolo/yolo.h"
#include "algoProcess.h"
#include "algo_internal.h"
#include "frame_pipeline.h"

/*======================== 取结果（dispatch_worker
 * 调用）========================*/

bool algorithm_take_results(int chnId, std::vector<AlgoResult> &out, cv::Mat &out_frame, int64_t &out_frame_id)
{
    out.clear();
    out_frame.release();
    out_frame_id = 0;
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return false;

    pthread_mutex_lock(&g_algo.channel_results[chnId].mtx);
    if (g_algo.channel_results[chnId].has_new)
    {
        out = std::move(g_algo.channel_results[chnId].data);
        out_frame = std::move(g_algo.channel_results[chnId].data_frame);
        out_frame_id = g_algo.channel_results[chnId].latest_seq;
        g_algo.channel_results[chnId].has_new = 0;
        pthread_mutex_unlock(&g_algo.channel_results[chnId].mtx);
        return true;
    }
    pthread_mutex_unlock(&g_algo.channel_results[chnId].mtx);
    return false;
}

/*======================== 初始化 ========================*/

int algorithm_init(const AppConfig &cfg)
{
    /* 初始化 pthread 同步原语 */
    pthread_rwlock_init(&g_algo.dispatch_mtx, nullptr);
    pthread_mutex_init(&g_algo.detect_classes_mtx, nullptr);
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        pthread_mutex_init(&g_algo.channel_results[i].mtx, nullptr);
        pthread_mutex_init(&g_algo.result_ready_mtx[i], nullptr);
        pthread_cond_init(&g_algo.result_ready_cv[i], nullptr);
        pthread_mutex_init(&g_algo.chn_reload_mtx[i], nullptr);
    }

    g_algo.obj_thresh.assign(MAX_CHANNEL_NUM, cfg.obj_thresh);
    g_algo.nms_thresh.assign(MAX_CHANNEL_NUM, cfg.nms_thresh);

    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        g_algo.detect_classes[i] = std::make_shared<const std::set<int>>();

    for (size_t i = 0; i < cfg.channels.size() && i < MAX_CHANNEL_NUM; ++i)
    {
        int runtime_chn = (int)i;
        g_algo.obj_thresh[runtime_chn] = cfg.channels[i].obj_thresh;
        g_algo.nms_thresh[runtime_chn] = cfg.channels[i].nms_thresh;
        g_algo.detect_classes[runtime_chn] = std::make_shared<const std::set<int>>(
            names_to_class_ids(cfg.channels[i].detect_classes, cfg.channels[i].label_path));
    }

    int core_masks[3] = {RKNN_NPU_CORE_0, RKNN_NPU_CORE_1, RKNN_NPU_CORE_2};
    int auto_model_instances = 0, loaded_models_count = 0;

    g_algo.model_registry.clear();
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        g_algo.result_dispatch_pending[i] = 0;

    for (size_t i = 0; i < cfg.channels.size() && i < MAX_CHANNEL_NUM; ++i)
    {
        const auto &chn_cfg = cfg.channels[i];
        int runtime_chn = (int)i;
        if (!chn_cfg.enable)
            continue;
        if (!config_utils::is_channel_infer_enabled(chn_cfg))
        {
            log_printf_threadsafe("[Algo] ch%d(id=%d) inference disabled, skip model init\n", runtime_chn, chn_cfg.id);
            continue;
        }

        try
        {
            int threads_for_chn = chn_cfg.threads > 0 ? chn_cfg.threads : 1;
            g_algo.models_per_chn[runtime_chn].clear();
            std::string type = chn_cfg.model_type;
            std::string label_path = chn_cfg.label_path.empty() ? cfg.label_path : chn_cfg.label_path;

            for (int t = 0; t < threads_for_chn; ++t)
            {
                int mask, core_id;
                if (chn_cfg.npu_core >= 0 && chn_cfg.npu_core <= 2)
                {
                    core_id = chn_cfg.npu_core;
                    mask = core_masks[core_id];
                }
                else
                {
                    core_id = auto_model_instances % 3;
                    mask = core_masks[core_id];
                    auto_model_instances++;
                }

                std::shared_ptr<ModelBase> model;
                if (t == 0)
                {
                    AlgoEngine::ModelKey mkey{type, chn_cfg.model_path, mask};
                    auto it = g_algo.model_registry.find(mkey);
                    if (it != g_algo.model_registry.end())
                    {
                        model = it->second;
                        log_printf_threadsafe("[Algo] Reusing %s instance for ch%d (Core %d) [shared]\n", type.c_str(),
                                              runtime_chn, core_id);
                    }
                    else
                    {
                        model = create_model(type, chn_cfg.model_path, label_path, mask, chn_cfg.obj_thresh,
                                             chn_cfg.nms_thresh);
                        if (!model)
                        {
                            printf("[Algo] Unsupported model_type '%s' for channel %d\n", type.c_str(), runtime_chn);
                            continue;
                        }
                        g_algo.model_registry[mkey] = model;
                        log_printf_threadsafe("[Algo] Created %s instance for ch%d (Core %d, thread 0/%d)\n",
                                              type.c_str(), runtime_chn, core_id, threads_for_chn);
                    }
                }
                else
                {
                    model = create_model(type, chn_cfg.model_path, label_path, mask, chn_cfg.obj_thresh,
                                         chn_cfg.nms_thresh);
                    if (!model)
                    {
                        printf("[Algo] Unsupported model_type '%s' for channel %d\n", type.c_str(), runtime_chn);
                        continue;
                    }
                    log_printf_threadsafe("[Algo] Created %s instance for ch%d (Core %d, "
                                          "thread %d/%d) [pipeline]\n",
                                          type.c_str(), runtime_chn, core_id, t, threads_for_chn);
                }

                if (loaded_models_count == 0)
                {
                    g_algo.input_w = model->input_width();
                    g_algo.input_h = model->input_height();
                }
                g_algo.models_per_chn[runtime_chn].push_back(model);
                loaded_models_count++;
            }
        }
        catch (const std::exception &e)
        {
            log_printf_threadsafe("[Algo] Load error ch%d: %s\n", runtime_chn, e.what());
        }
    }

    if (loaded_models_count == 0)
        log_printf_threadsafe("[Algo] No active logic models available\n");

    if (!g_algo.running)
    {
        g_algo.running = 1;
        g_algo.max_queue_size = std::max(1, cfg.queue_size);
        g_algo.task_queues.clear();
        int q_count = 0;
        for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        {
            if (!g_algo.models_per_chn[i].empty())
            {
                auto tq = std::make_unique<TaskQueue>();
                pthread_mutex_init(&tq->mtx, nullptr);
                pthread_cond_init(&tq->cv, nullptr);
                g_algo.task_queues.push_back(std::move(tq));
                q_count++;
            }
        }
        auto now = std::chrono::steady_clock::now();
        uint64_t now_ms = algo_steady_now_ms();
        for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        {
            g_fps[i].init(now);
            g_perf[i].last_log_ms = now_ms;
        }

        /* 创建 worker 线程 */
        int q_idx = 0;
        for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        {
            if (!g_algo.models_per_chn[i].empty())
            {
                TaskQueue *tq = g_algo.task_queues[q_idx].get();
                for (const auto &model : g_algo.models_per_chn[i])
                {
                    WorkerArg *wa = new WorkerArg{i, tq, model};
                    pthread_t tid;
                    pthread_create(&tid, nullptr, worker_thread_func, wa);
                    g_algo.worker_tids.push_back(tid);
                    log_printf_threadsafe("[Algo] infer_worker created: ch%d tid=%lu\n", i, (unsigned long)tid);
                }
                g_algo.channel_results[i].latest_seq = 0;
                q_idx++;
            }
        }
    }
    return 0;
}

/*======================== 反初始化 ========================*/

void algorithm_deinit()
{
    g_algo.running = 0;
    for (auto &tq : g_algo.task_queues)
    {
        pthread_cond_broadcast(&tq->cv);
    }
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        pthread_cond_broadcast(&g_algo.result_ready_cv[i]);

    for (pthread_t tid : g_algo.worker_tids)
        pthread_join(tid, nullptr);
    g_algo.worker_tids.clear();

    /* 排干所有正在临界区内的 algorithm_process_mat 调用者 */
    pthread_rwlock_wrlock(&g_algo.dispatch_mtx);

    for (auto &tq : g_algo.task_queues)
    {
        pthread_mutex_destroy(&tq->mtx);
        pthread_cond_destroy(&tq->cv);
    }
    g_algo.task_queues.clear();
    for (auto &models : g_algo.models_per_chn)
        models.clear();
    g_algo.model_registry.clear();

    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        pthread_mutex_destroy(&g_algo.channel_results[i].mtx);
        pthread_mutex_destroy(&g_algo.result_ready_mtx[i]);
        pthread_cond_destroy(&g_algo.result_ready_cv[i]);
        pthread_mutex_destroy(&g_algo.chn_reload_mtx[i]);
    }
    pthread_mutex_destroy(&g_algo.detect_classes_mtx);

    pthread_rwlock_unlock(&g_algo.dispatch_mtx);
    pthread_rwlock_destroy(&g_algo.dispatch_mtx);
}

/*======================== 帧入队（videoOutHandle
 * 调用）========================*/

int algorithm_process_mat(int chnId, cv::Mat &&frame, int fd, int srcW, int srcH, int srcFmt, int srcStrH, int srcStrV,
                          int64_t frame_seq)
{
    if (!g_algo.running)
        return -1;
    if (frame.empty() && fd < 0)
        return -1;

    pthread_rwlock_rdlock(&g_algo.dispatch_mtx);
    if (!g_algo.running)
    {
        pthread_rwlock_unlock(&g_algo.dispatch_mtx);
        return -1;
    }
    if (g_algo.task_queues.empty())
    {
        pthread_rwlock_unlock(&g_algo.dispatch_mtx);
        return -1;
    }
    if (chnId >= 0 && chnId < MAX_CHANNEL_NUM && g_algo.chn_reload_stop[chnId])
    {
        pthread_rwlock_unlock(&g_algo.dispatch_mtx);
        return -1;
    }

    int q_idx = 0;
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        if (!g_algo.models_per_chn[i].empty())
        {
            if (i == chnId)
            {
                TaskQueue &tq = *g_algo.task_queues[q_idx];
                pthread_mutex_lock(&tq.mtx);
                if (tq.q.size() >= (size_t)g_algo.max_queue_size)
                {
                    pthread_mutex_unlock(&tq.mtx);
                    pthread_rwlock_unlock(&g_algo.dispatch_mtx);
                    return 0;
                }

                AlgoTask task;
                task.chnId = chnId;
                task.img = std::move(frame);
                task.enqueue_tp = std::chrono::steady_clock::now();
                task.frame_seq = frame_seq > 0 ? frame_seq : ++g_fps[chnId].frame_seq;
                if (fd >= 0)
                    task.src_buf = rga_import_src_fd(fd, srcW, srcH, srcStrH, srcStrV, srcFmt);
                task.srcW = srcW;
                task.srcH = srcH;
                task.srcFmt = srcFmt;
                task.srcStrH = srcStrH;
                task.srcStrV = srcStrV;
                tq.q.push(std::move(task));
                pthread_cond_signal(&tq.cv);
                pthread_mutex_unlock(&tq.mtx);
                pthread_rwlock_unlock(&g_algo.dispatch_mtx);
                return 1;
            }
            q_idx++;
        }
    }
    pthread_rwlock_unlock(&g_algo.dispatch_mtx);
    return 0;
}

/*======================== 查询接口 ========================*/

int algorithm_get_input_w()
{
    return g_algo.input_w;
}
int algorithm_get_input_h()
{
    return g_algo.input_h;
}

float algorithm_get_infer_fps(int chnId)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return 0.0f;
    return g_fps[chnId].fps;
}

bool algorithm_wait_result(int chnId, int timeout_ms)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return false;

    pthread_mutex_lock(&g_algo.result_ready_mtx[chnId]);
    if (!g_algo.result_dispatch_pending[chnId] && g_algo.running)
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
        pthread_cond_timedwait(&g_algo.result_ready_cv[chnId], &g_algo.result_ready_mtx[chnId], &ts);
    }
    int got = g_algo.result_dispatch_pending[chnId] != 0 || !g_algo.running;
    if (got && g_algo.result_dispatch_pending[chnId])
    {
        g_algo.result_dispatch_pending[chnId] = 0;
        pthread_mutex_unlock(&g_algo.result_ready_mtx[chnId]);
        return true;
    }
    pthread_mutex_unlock(&g_algo.result_ready_mtx[chnId]);
    return false;
}

/*======================== 热重载接口 ========================*/

void algorithm_update_thresh(int chnId, float obj_thresh, float nms_thresh)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return;
    printf("[AlgoProcess] Updating thresh for channel %d: obj=%.2f, nms=%.2f\n", chnId, obj_thresh, nms_thresh);
    g_algo.obj_thresh[chnId] = obj_thresh;
    g_algo.nms_thresh[chnId] = nms_thresh;

    for (auto &model : g_algo.models_per_chn[chnId])
    {
        if (!model)
            continue;
        float min_obj = obj_thresh, min_nms = nms_thresh;
        for (int c = 0; c < MAX_CHANNEL_NUM; ++c)
            for (auto &m : g_algo.models_per_chn[c])
                if (m.get() == model.get())
                {
                    min_obj = std::min(min_obj, g_algo.obj_thresh[c]);
                    min_nms = std::min(min_nms, g_algo.nms_thresh[c]);
                }
        model->set_thresh(min_obj, min_nms);
        printf("[AlgoProcess] Model instance updated for channel %d "
               "(model_thresh=%.2f/%.2f)\n",
               chnId, min_obj, min_nms);
    }
}

void algorithm_update_detect_classes(int chnId, const std::vector<std::string> &class_names)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return;
    std::string label_path;
    if (g_pCtrl)
        label_path = g_pCtrl->config.channels[chnId].label_path;
    auto ids = std::make_shared<const std::set<int>>(names_to_class_ids(class_names, label_path));
    pthread_mutex_lock(&g_algo.detect_classes_mtx);
    g_algo.detect_classes[chnId] = std::move(ids);
    pthread_mutex_unlock(&g_algo.detect_classes_mtx);
}

void algorithm_reload_channel_model(int chnId, const ChannelConfig &new_cfg)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM || !g_algo.running)
        return;

    pthread_rwlock_wrlock(&g_algo.dispatch_mtx);

    /* 找到此通道的任务队列（定义在 algo_engine.cpp，声明在 algo_internal.h）*/
    int q_idx = get_queue_idx_for_chn(chnId);
    if (q_idx < 0 || q_idx >= (int)g_algo.task_queues.size())
    {
        log_printf_threadsafe("[Algo] ch%d has no task queue, skipping model reload\n", chnId);
        pthread_rwlock_unlock(&g_algo.dispatch_mtx);
        return;
    }
    TaskQueue *tq = g_algo.task_queues[q_idx].get();

    log_printf_threadsafe("[Algo] Reloading model for ch%d: type=%s path=%s\n", chnId, new_cfg.model_type.c_str(),
                          new_cfg.model_path.c_str());

    int worker_start = 0;
    for (int i = 0; i < chnId; ++i)
        worker_start += (int)g_algo.models_per_chn[i].size();
    int worker_count = (int)g_algo.models_per_chn[chnId].size();

    g_algo.chn_reload_stop[chnId] = 1;
    pthread_cond_broadcast(&tq->cv);
    for (int w = worker_start; w < worker_start + worker_count; ++w)
        pthread_join(g_algo.worker_tids[w], nullptr);

    { /* 清空队列残留任务 */
        pthread_mutex_lock(&tq->mtx);
        while (!tq->q.empty())
            tq->q.pop();
        pthread_mutex_unlock(&tq->mtx);
    }

    auto restart_workers = [&](const std::vector<std::shared_ptr<ModelBase>> &new_models) {
        g_algo.models_per_chn[chnId] = new_models;
        {
            pthread_mutex_lock(&g_algo.channel_results[chnId].mtx);
            g_algo.channel_results[chnId].data.clear();
            g_algo.channel_results[chnId].has_new = 0;
            pthread_mutex_unlock(&g_algo.channel_results[chnId].mtx);
        }
        g_fps[chnId].fps = 0.0f;
        g_algo.chn_reload_stop[chnId] = 0;
        for (int w = worker_start; w < worker_start + worker_count; ++w)
        {
            int m_idx = w - worker_start;
            WorkerArg *wa = new WorkerArg{
                chnId, tq, m_idx < (int)new_models.size() ? new_models[m_idx] : std::shared_ptr<ModelBase>()};
            pthread_create(&g_algo.worker_tids[w], nullptr, worker_thread_func, wa);
        }
    };

    if (new_cfg.model_path.empty() || new_cfg.model_type.empty())
    {
        restart_workers(std::vector<std::shared_ptr<ModelBase>>(worker_count));
        log_printf_threadsafe("[Algo] ch%d inference disabled after reload\n", chnId);
        pthread_rwlock_unlock(&g_algo.dispatch_mtx);
        return;
    }

    int core_masks[3] = {RKNN_NPU_CORE_0, RKNN_NPU_CORE_1, RKNN_NPU_CORE_2};
    std::vector<std::shared_ptr<ModelBase>> new_models;
    std::string label_path = new_cfg.label_path;
    if (label_path.empty() && g_pCtrl)
        label_path = g_pCtrl->config.label_path;

    try
    {
        for (int t = 0; t < worker_count; ++t)
        {
            int mask;
            if (new_cfg.npu_core >= 0 && new_cfg.npu_core <= 2)
                mask = core_masks[new_cfg.npu_core];
            else
                mask = core_masks[(worker_start + t) % 3];
            auto model = create_model(new_cfg.model_type, new_cfg.model_path, label_path, mask, new_cfg.obj_thresh,
                                      new_cfg.nms_thresh);
            if (!model)
            {
                log_printf_threadsafe("[Algo] Unsupported model_type '%s' for ch%d\n", new_cfg.model_type.c_str(),
                                      chnId);
                continue;
            }
            new_models.push_back(model);
        }
    }
    catch (const std::exception &e)
    {
        log_printf_threadsafe("[Algo] Model load error ch%d: %s\n", chnId, e.what());
        g_algo.chn_reload_stop[chnId] = 0;
        pthread_rwlock_unlock(&g_algo.dispatch_mtx);
        return;
    }

    if (new_models.empty())
    {
        restart_workers(std::vector<std::shared_ptr<ModelBase>>(worker_count));
        log_printf_threadsafe("[Algo] No models created for ch%d, switched to no-infer mode\n", chnId);
    }
    else
    {
        restart_workers(new_models);
        log_printf_threadsafe("[Algo] ch%d model reload complete\n", chnId);
    }
    pthread_rwlock_unlock(&g_algo.dispatch_mtx);
}
