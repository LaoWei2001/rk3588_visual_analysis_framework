/**
 * @file inference_internal.h
 * @brief 推理引擎内部类型 + 全局状态声明
 *
 * 仅供 inference_engine.cpp / inference_executor.cpp 使用。
 * 外部模块请用 inference_engine.h。
 *
 * 文件职责分工:
 *   inference_executor.cpp  — g_inference/g_fps/g_perf 定义 + inference_worker_thread + 私有辅助函数
 *   inference_engine.cpp  — 公有 API 实现 (inference_init / process_source / take_results …)
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "application/config/config.h"
#include "common/performance_metrics.h"
#include "inference_engine.h"
#include "pipeline/frame_transform.h" /* RgaImportedBuffer / LazyVideoFrame */
#include "vision/models/model_group.h"
#include <coordinates.h>

/*======================== 内部任务结构 ========================*/

#include "inference_task.h"

struct InferenceChannelResult
{
    std::vector<AlgoResult> data;
    std::shared_ptr<LazyVideoFrame> frame;
    uint64_t frame_steady_ms{0};
    uint64_t frame_unix_ms{0};
    pthread_mutex_t mtx;
    int64_t latest_seq{0};
    int has_new{0};
};

struct InferenceTaskQueue
{
    std::queue<InferenceTask> q;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
};

/*======================== FPS 跟踪器（每通道）========================*/

struct FpsTracker
{
    FrameRateCounter rate;
    std::atomic<int64_t> frame_seq{0};

    void init()
    {
        rate.reset();
        frame_seq.store(0);
    }
    void tick()
    {
        rate.tick();
    }
    float value() const
    {
        return rate.value();
    }
    void reset_rate()
    {
        rate.reset();
    }
    int64_t next_frame_seq()
    {
        return ++frame_seq;
    }
};

/*======================== 推理引擎主状态（模块级单例）========================*/

struct InferenceRuntime
{
    pthread_rwlock_t dispatch_mtx;

    std::vector<std::vector<std::shared_ptr<ModelGroup>>> model_groups_per_chn{MAX_CHANNEL_NUM};

    std::atomic<float> obj_thresh[MAX_CHANNEL_NUM]{};
    std::atomic<float> nms_thresh[MAX_CHANNEL_NUM]{};

    std::shared_ptr<const std::set<int>> detect_classes[MAX_CHANNEL_NUM];
    pthread_mutex_t detect_classes_mtx;

    std::vector<std::unique_ptr<InferenceTaskQueue>> task_queues;
    std::atomic<bool> running{false};
    std::atomic<bool> chn_reload_stop[MAX_CHANNEL_NUM]{};
    // Requested placement of active contexts; retained when reload rolls back.
    std::atomic<int> auto_core_plan[MAX_CHANNEL_NUM]{};
    std::vector<pthread_t> worker_tids;
    std::vector<unsigned char> worker_started;
    int max_queue_size{1};

    InferenceChannelResult channel_results[MAX_CHANNEL_NUM];

    pthread_cond_t result_ready_cv[MAX_CHANNEL_NUM];
    pthread_mutex_t result_ready_mtx[MAX_CHANNEL_NUM];
    int result_dispatch_pending[MAX_CHANNEL_NUM]{};
};

/** @brief Worker 线程入口参数（heap-alloc，worker 内部 delete）。*/
struct InferenceWorkerArgs
{
    int chnId;
    InferenceTaskQueue *tq;
    std::shared_ptr<ModelGroup> model_group;
};

/*======================== 全局状态（定义在 inference_executor.cpp）========================*/

extern InferenceRuntime g_inference;
extern FpsTracker g_fps[MAX_CHANNEL_NUM];
extern InferencePerfCounters g_perf[MAX_CHANNEL_NUM];

static constexpr uint64_t PERF_LOG_WINDOW_MS = 5000;

/** @brief 单调毫秒时间戳（多个文件复用，inline 避免多重定义）。*/
static inline uint64_t inference_steady_now_ms(void)
{
    auto now = std::chrono::steady_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

/*======================== 视频工作线程辅助函数 ========================*/

/** @brief 返回 chnId 对应的任务队列下标；找不到返回 -1。*/
int get_queue_idx_for_chn(int chnId);

/** @brief 推理 worker 线程入口（通过 pthread_create 调用）。*/
void *inference_worker_thread(void *arg);
