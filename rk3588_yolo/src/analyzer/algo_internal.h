/**
 * @file algo_internal.h
 * @brief 推理引擎内部类型 + 全局状态声明
 *
 * 仅供 algoProcess.cpp / algo_engine.cpp 使用。
 * 外部模块请用 algoProcess.h。
 *
 * 文件职责分工:
 *   algo_engine.cpp  — g_algo/g_fps/g_perf 定义 + worker_thread_func +
 * 私有辅助函数 algoProcess.cpp  — 公有 API 实现 (algorithm_init / process_mat /
 * take_results …)
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "../config/config.h"
#include "../yolo/yolo.h" /* ModelBase */
#include "algoProcess.h"
#include "frame_pipeline.h" /* RgaImportedBuffer */

/*======================== 内部任务结构 ========================*/

struct AlgoTask
{
    int chnId;
    cv::Mat img;
    std::chrono::steady_clock::time_point enqueue_tp;
    int64_t frame_seq;
    std::shared_ptr<RgaImportedBuffer> src_buf;
    int srcW, srcH, srcFmt, srcStrH, srcStrV;
};

struct ChannelResult
{
    std::vector<AlgoResult> data;
    cv::Mat data_frame;
    pthread_mutex_t mtx;
    int64_t latest_seq{0};
    int has_new{0};
};

struct TaskQueue
{
    std::queue<AlgoTask> q;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
};

/*======================== 性能计数器（每通道，worker
 * 独占，无需原子）========================*/

struct PerfCounters
{
    uint64_t wait_us{0}, lock_us{0}, pre_us{0}, npu_us{0};
    uint64_t post_us{0}, filter_nms_us{0}, total_us{0}, samples{0};
    uint64_t last_log_ms{0};

    void accumulate(uint64_t wait, uint64_t lock, uint64_t pre, uint64_t npu, uint64_t post, uint64_t filter_nms,
                    uint64_t total)
    {
        wait_us += wait;
        lock_us += lock;
        pre_us += pre;
        npu_us += npu;
        post_us += post;
        filter_nms_us += filter_nms;
        total_us += total;
        samples++;
    }

    struct Snapshot
    {
        uint64_t samples, wait, lock, pre, npu, post, filter_nms, total;
    };
    Snapshot reset()
    {
        Snapshot s{samples, wait_us, lock_us, pre_us, npu_us, post_us, filter_nms_us, total_us};
        wait_us = lock_us = pre_us = npu_us = post_us = filter_nms_us = total_us = samples = 0;
        return s;
    }
};

/*======================== FPS 跟踪器（每通道）========================*/

struct FpsTracker
{
    std::chrono::steady_clock::time_point last_ts;
    int counter{0};
    float fps{0.0f};
    int64_t frame_seq{0};

    void init(std::chrono::steady_clock::time_point now)
    {
        last_ts = now;
        counter = 0;
        fps = 0.0f;
        frame_seq = 0;
    }

    void tick()
    {
        counter++;
        auto now_ts = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now_ts - last_ts).count();
        if (elapsed >= 1000)
        {
            fps = (1000.0f * counter) / (float)elapsed;
            counter = 0;
            last_ts = now_ts;
        }
    }
};

/*========================
 * 推理引擎主状态（模块级单例）========================*/

struct AlgoEngine
{
    pthread_rwlock_t dispatch_mtx;

    std::vector<std::vector<std::shared_ptr<ModelBase>>> models_per_chn{MAX_CHANNEL_NUM};
    int input_w{640}, input_h{640};

    std::vector<float> obj_thresh{MAX_CHANNEL_NUM, 0.4f};
    std::vector<float> nms_thresh{MAX_CHANNEL_NUM, 0.45f};

    std::shared_ptr<const std::set<int>> detect_classes[MAX_CHANNEL_NUM];
    pthread_mutex_t detect_classes_mtx;

    std::vector<std::unique_ptr<TaskQueue>> task_queues;
    volatile int running{0};
    volatile int chn_reload_stop[MAX_CHANNEL_NUM]{};
    pthread_mutex_t chn_reload_mtx[MAX_CHANNEL_NUM];
    std::vector<pthread_t> worker_tids;
    int max_queue_size{1};

    ChannelResult channel_results[MAX_CHANNEL_NUM];

    pthread_cond_t result_ready_cv[MAX_CHANNEL_NUM];
    pthread_mutex_t result_ready_mtx[MAX_CHANNEL_NUM];
    volatile int result_dispatch_pending[MAX_CHANNEL_NUM]{};

    using ModelKey = std::tuple<std::string, std::string, int>;
    std::map<ModelKey, std::shared_ptr<ModelBase>> model_registry;
};

/** @brief Worker 线程入口参数（heap-alloc，worker 内部 delete）。*/
struct WorkerArg
{
    int chnId;
    TaskQueue *tq;
    std::shared_ptr<ModelBase> model;
};

/*======================== 全局状态（定义在
 * algo_engine.cpp）========================*/

extern AlgoEngine g_algo;
extern FpsTracker g_fps[MAX_CHANNEL_NUM];
extern PerfCounters g_perf[MAX_CHANNEL_NUM];

static constexpr uint64_t PERF_LOG_WINDOW_MS = 5000;

/** @brief 单调毫秒时间戳（多个文件复用，inline 避免多重定义）。*/
static inline uint64_t algo_steady_now_ms(void)
{
    auto now = std::chrono::steady_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

/*======================== 跨文件函数声明（定义在
 * algo_engine.cpp）========================*/

/** @brief 返回 chnId 对应的任务队列下标；找不到返回 -1。*/
int get_queue_idx_for_chn(int chnId);

/** @brief 按标签名列表解析标签文件得到 class_id 集合。*/
std::set<int> names_to_class_ids(const std::vector<std::string> &names, const std::string &label_path);

/** @brief 按 model_type 创建对应的 ModelBase 子类实例。*/
std::shared_ptr<ModelBase> create_model(const std::string &type, const std::string &model_path,
                                        const std::string &label_path, int core_mask, float obj_thresh,
                                        float nms_thresh);

/** @brief 推理 worker 线程入口（通过 pthread_create 调用）。*/
void *worker_thread_func(void *arg);
