/**
 * @file algo_engine.cpp
 * @brief 推理引擎核心 — 全局状态 + worker 线程 + 私有辅助函数
 *
 * 职责（仅此文件）:
 *   - g_algo / g_fps / g_perf 的唯一定义
 *   - nms_inplace / get_queue_idx_for_chn — 引擎私有辅助（static，不对外）
 *   - names_to_class_ids / create_model   — 跨文件辅助（声明在
 * algo_internal.h）
 *   - worker_thread_func                  — NPU 推理 worker（声明在
 * algo_internal.h）
 *
 * 公有 API（algorithm_init / algorithm_process_mat / …）在 algoProcess.cpp。
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "../core/app_ctrl.h"
#include "../core/pause_ctrl.h"
#include "../system.h"
#include "../yolo/yolo.h"
#include "../yolo/yolopose.h"
#include "../yolo/yoloseg.h"
#include "../yolo/yolov8det.h"
#include "algoProcess.h"
#include "algo_internal.h"
#include "frame_pipeline.h"

/* compute_iou 声明在 algoProcess.h / yolo.h 全局命名空间，此处显式引入供
 * nms_inplace 使用 */
using ::compute_iou;

/*======================== 全局状态定义（唯一定义，其余文件通过 extern
 * 访问）========================*/

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
        return std::make_shared<YoloPose>(model_path, core_mask, obj_thresh, nms_thresh);
    if (type == "yolov5_seg")
        return std::make_shared<YoloSeg>(model_path, label_path, core_mask, obj_thresh, nms_thresh);
    if (type == "yolov5")
        return std::make_shared<YOLO>(model_path, label_path, core_mask, obj_thresh, nms_thresh);
    if (type == "yolov8_det")
        return std::make_shared<YoloV8Det>(model_path, label_path, core_mask, obj_thresh, nms_thresh);
    return nullptr;
}

/*======================== NPU 推理 Worker 线程 ========================*/

void *worker_thread_func(void *arg)
{
    WorkerArg *wa = (WorkerArg *)arg;
    int chnId = wa->chnId;
    TaskQueue *tq_ptr = wa->tq;
    std::shared_ptr<ModelBase> model_ptr = wa->model;
    delete wa;

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

        int model_fd = model->get_input_fd();

        auto lock_before = std::chrono::steady_clock::now();
        pthread_mutex_lock(&model->infer_mtx);
        auto lock_after = std::chrono::steady_clock::now();
        float lock_wait_ms = std::chrono::duration<float, std::milli>(lock_after - lock_before).count();

        if (task.src_buf && task.src_buf->handle != 0 && model_fd >= 0)
        {
            auto pre_begin = std::chrono::steady_clock::now();
            int cached_handle = model->get_input_rga_handle();
            bool rga_ok = rga_convert_resize_handle(task.chnId, *task.src_buf, model_fd, model->input_width(),
                                                    model->input_height(), model->input_width(), model->input_height(),
                                                    RK_FORMAT_RGB_888, cached_handle);
            auto pre_end = std::chrono::steady_clock::now();
            perf.preprocess_ms = std::chrono::duration<float, std::milli>(pre_end - pre_begin).count();
            if (rga_ok)
                ret = model->infer_zero_copy(results, &perf);
        }

        if (!ret && !task.img.empty())
            ret = model->infer(task.img, results, &perf);

        pthread_mutex_unlock(&model->infer_mtx);

        if (ret)
        {
            auto filter_begin = std::chrono::steady_clock::now();
            float obj_thresh_v = g_algo.obj_thresh[task.chnId];
            float nms_thresh_v = g_algo.nms_thresh[task.chnId];

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

            const size_t MAX_DET_PER_FRAME = 20;
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
            uint64_t last_ms = g_perf[task.chnId].last_log_ms;
            if (last_ms == 0)
                g_perf[task.chnId].last_log_ms = now_ms;
            else if (now_ms - last_ms >= PERF_LOG_WINDOW_MS)
            {
                g_perf[task.chnId].last_log_ms = now_ms;
                auto snap = g_perf[task.chnId].reset();
                if (snap.samples > 0 && g_pCtrl && g_pCtrl->config.performance_display)
                {
                    float div = (float)snap.samples * 1000.0f;
                    log_printf_threadsafe("[Perf][ch%02d][5s] fps=%.1f | "
                                          "wait_q=%.2f lock=%.2f pre=%.2f npu=%.2f post=%.2f nms=%.2f | "
                                          "total=%.2fms\n",
                                          task.chnId, g_fps[task.chnId].fps, (float)snap.wait / div,
                                          (float)snap.lock / div, (float)snap.pre / div, (float)snap.npu / div,
                                          (float)snap.post / div, (float)snap.filter_nms / div,
                                          (float)snap.total / div);
                }
            }
        }
    }
    return nullptr;
}
