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
#include <map>
#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "common/performance_metrics.h"
#include "common/business_coordinates.h"
#include "config/config.h"
#include "inference_engine.h"
#include "pipeline/frame_transform.h" /* RgaImportedBuffer / LazyVideoFrame */
#include "yolo/yolo.h"                /* ModelBase */

/*======================== 内部任务结构 ========================*/

enum class InferenceRoiMode : uint8_t
{
    FullFrame = 0,
    RoiOnly,
    FullPlusRoi,
    FullFrameRoiFilter,
};

struct InferenceTask
{
    int chnId;
    std::shared_ptr<LazyVideoFrame> frame;
    std::chrono::steady_clock::time_point enqueue_tp;
    int64_t frame_seq;
    uint64_t frame_steady_ms;
    uint64_t frame_unix_ms;
    std::shared_ptr<RgaImportedBuffer> src_buf;
    int srcW, srcH, srcFmt, srcStrH, srcStrV;
    InferenceRoiMode roi_mode = InferenceRoiMode::FullFrame;
    InferenceRoiConfig roi_config;
    cv::Rect source_roi; /* 用户区域的源图包围盒；实际取景由 resize_mode 在 worker 中计算。 */
};

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
    void tick() { rate.tick(); }
    float value() const { return rate.value(); }
    void reset_rate() { rate.reset(); }
    int64_t next_frame_seq() { return ++frame_seq; }
};

/*======================== 推理引擎主状态（模块级单例）========================*/

struct InferenceRuntime
{
    pthread_rwlock_t dispatch_mtx;

    std::vector<std::vector<std::shared_ptr<ModelBase>>> models_per_chn{MAX_CHANNEL_NUM};
    /* 兼容内部字段名：这是业务画布，不是某个模型的输入。编译期固定，模型不可改写。 */
    static constexpr int input_w = business_coordinates::WIDTH;
    static constexpr int input_h = business_coordinates::HEIGHT;

    std::atomic<float> obj_thresh[MAX_CHANNEL_NUM]{};
    std::atomic<float> nms_thresh[MAX_CHANNEL_NUM]{};

    std::shared_ptr<const std::set<int>> detect_classes[MAX_CHANNEL_NUM];
    pthread_mutex_t detect_classes_mtx;

    std::vector<std::unique_ptr<InferenceTaskQueue>> task_queues;
    std::atomic<bool> running{false};
    std::atomic<bool> chn_reload_stop[MAX_CHANNEL_NUM]{};
    pthread_mutex_t chn_reload_mtx[MAX_CHANNEL_NUM];
    std::vector<pthread_t> worker_tids;
    std::vector<unsigned char> worker_started;
    int max_queue_size{1};

    InferenceChannelResult channel_results[MAX_CHANNEL_NUM];

    pthread_cond_t result_ready_cv[MAX_CHANNEL_NUM];
    pthread_mutex_t result_ready_mtx[MAX_CHANNEL_NUM];
    int result_dispatch_pending[MAX_CHANNEL_NUM]{};

    using ModelKey = std::tuple<std::string, std::string, int>;
    std::map<ModelKey, std::shared_ptr<ModelBase>> model_registry;
};

/** @brief Worker 线程入口参数（heap-alloc，worker 内部 delete）。*/
struct InferenceWorkerArgs
{
    int chnId;
    InferenceTaskQueue *tq;
    std::shared_ptr<ModelBase> model;
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

/*======================== 跨文件函数声明（定义在 inference_executor.cpp）========================*/

/** @brief 返回 chnId 对应的任务队列下标；找不到返回 -1。*/
int get_queue_idx_for_chn(int chnId);

/** @brief 按标签名列表解析标签文件得到 class_id 集合。*/
std::set<int> names_to_class_ids(const std::vector<std::string> &names, const std::string &label_path);

/** @brief 按 model_type 创建对应的 ModelBase 子类实例。*/
std::shared_ptr<ModelBase> create_inference_model(const std::string &type, const std::string &model_path,
                                                  const std::string &label_path, int core_mask, float obj_thresh,
                                                  float nms_thresh);

/** @brief 推理 worker 线程入口（通过 pthread_create 调用）。*/
void *inference_worker_thread(void *arg);
