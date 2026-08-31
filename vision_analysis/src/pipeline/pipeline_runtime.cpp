/**
 * @file pipeline.cpp
 * @brief 分析器模块 — 共享状态定义 + 生命周期管理
 *
 * 本文件仅负责:
 *   1. 定义跨文件共享的 extern 变量
 *        g_display_queues   — 显示单槽队列（frame_inlet + display_pipeline 共用）
 *        g_traditional_logic_queues — 非 NPU 逻辑单槽最新帧队列
 *        g_dispatch_running — pipeline_dispatch_worker 退出信号
 *        g_process_mtx   — 同通道 process_channel_results 串行锁
 *   2. 发布不可变 AppRuntimeSnapshot — 配置/ROI 热更新与逐帧 logic 解耦
 *   3. pipeline_init / pipeline_deinit — 模块启动 / 关闭
 *   4. pipeline_wake_display_threads / pipeline_destroy_display_queues
 *        供 main 在线程退出前调用
 *   5. pipeline_get_display/dispatch_thread_count/id
 *        供 main 决定创建多少线程、各线程对应哪个通道
 *
 * 各路径实现已拆分到独立文件（均通过 pipeline_internal.h 共享状态）:
 *   frame_inlet.cpp       — pipeline_submit_frame + FPS 节流 + RGA 转换 + 统计
 *   channel_pipeline.cpp  — 跟踪器 + process_channel_results + invoke_channel_logic
 *   result_dispatch.cpp   — pipeline_dispatch_worker
 *   display_pipeline.cpp  — display_worker_thread
 *   traditional_logic_worker.cpp — pipeline_logic_worker
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <pthread.h>
#include <string>
#include <utility>
#include <vector>

#include "recorder/event_video_recorder.h"
#include "inference/inference_engine.h"
#include "pipeline_runtime.h"
#include "pipeline_internal.h" /* DisplayTask/DisplayQueue 定义、extern 声明、时间辅助 */
#include "logic/core/channel_logic.h"
#include "logic/core/global_logic.h"
#include "runtime/app_ctrl.h"
#include "common/logging.h"

/*======================== 共享 extern 变量定义 ========================*/
/* 声明在 pipeline_internal.h（extern），此处给出唯一定义。 */

DisplayQueue g_display_queues[MAX_CHANNEL_NUM];
TraditionalLogicQueue g_traditional_logic_queues[MAX_CHANNEL_NUM];
std::atomic<bool> g_dispatch_running{false};
pthread_mutex_t g_process_mtx[MAX_CHANNEL_NUM];
static std::vector<int> g_display_channel_ids;
static std::vector<int> g_logic_channel_ids;
static bool g_process_mutexes_initialized = false;
static bool g_pipeline_initialized = false;

/*======================== 初始化 / 反初始化 ========================*/

int pipeline_init(void)
{
    /* 跟踪器全部清零（各通道首次调用 process_channel_results 时惰性创建）*/
    trackers_init();

    /* 热更新发布要与逐帧逻辑共用这些锁，必须先于首代运行快照初始化。 */
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        pthread_mutex_init(&g_process_mtx[i], nullptr);
    g_process_mutexes_initialized = true;

    if (inference_init(g_pCtrl->config) != 0)
    {
        for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
            pthread_mutex_destroy(&g_process_mtx[i]);
        g_process_mutexes_initialized = false;
        return -1;
    }
    g_pCtrl->inputW = inference_get_input_w();
    g_pCtrl->inputH = inference_get_input_h();

    if (!pipeline_publish_runtime_snapshot(g_pCtrl->config, g_pCtrl->config_generation))
    {
        inference_deinit();
        for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
            pthread_mutex_destroy(&g_process_mtx[i]);
        g_process_mutexes_initialized = false;
        return -1;
    }
    global_logic_start_all(g_pCtrl->config.global_logics);

    /* 启用分发线程运行标志 */
    g_dispatch_running.store(true);

    /* 每个启用通道都预留一个传统逻辑 worker。通常推理通道的 worker
     * 始终休眠，但这样运行时 infer_toggle 关闭 NPU 后仍能无缝转入异步 CV，
     * 不会回退到 appsink 回调同步执行。 */
    g_logic_channel_ids.clear();
    for (int channel_id = 0; channel_id < MAX_CHANNEL_NUM; ++channel_id)
    {
        TraditionalLogicQueue &queue = g_traditional_logic_queues[channel_id];
        pthread_mutex_init(&queue.mtx, nullptr);
        pthread_cond_init(&queue.cv, nullptr);
        queue.has_task = false;
        queue.task = TraditionalLogicTask{};
    }
    for (const auto &channel : g_pCtrl->config.channels)
    {
        if (!channel.enable || channel.id < 0 || channel.id >= MAX_CHANNEL_NUM)
            continue;
        g_logic_channel_ids.push_back(channel.id);
    }

    /* 纯分析模式不创建显示线程，也不预留每通道三槽 1080p 帧池。 */
    g_display_channel_ids.clear();
    if (app_ctrl_get_enable_disp() || app_ctrl_get_enable_rtsp())
        for (const auto &channel : g_pCtrl->config.channels)
            g_display_channel_ids.push_back(channel.id);
    /* 初始化显示队列互斥量 / 条件变量 / 帧池 */
    for (int channel_id : g_display_channel_ids)
    {
        pthread_mutex_init(&g_display_queues[channel_id].mtx, nullptr);
        pthread_cond_init(&g_display_queues[channel_id].cv, nullptr);
        g_display_queues[channel_id].has_task = 0;
        g_display_queues[channel_id].reset_fps_pending = true;
        g_display_queues[channel_id].pool.init(); /* 预分配三槽帧缓冲（≈9 MB/通道，NV12 1080p）*/
    }

    g_pipeline_initialized = true;

    printf("[Pipeline] initialized. Thread creation managed by main.\n");
    return 0;
}

void pipeline_request_stop(void)
{
    g_dispatch_running.store(false);
    inference_request_stop();
    for (int channel_id : g_logic_channel_ids)
    {
        pthread_mutex_lock(&g_traditional_logic_queues[channel_id].mtx);
        pthread_cond_broadcast(&g_traditional_logic_queues[channel_id].cv);
        pthread_mutex_unlock(&g_traditional_logic_queues[channel_id].mtx);
    }
    pipeline_wake_display_threads();
}

void pipeline_deinit(void)
{
    if (!g_pipeline_initialized)
        return;
    pipeline_request_stop();
    inference_deinit();

    global_logic_stop_all();
    trackers_deinit();
    g_pipeline_initialized = false;
}

/*======================== 显示线程辅助（供 main 调用）========================*/

/**
 * @brief 广播所有显示队列条件变量，使阻塞中的 display_worker_thread 能检测到退出信号。
 *
 * main 在设置 g_pCtrl->isRunning = 0 之后调用，防止线程永久阻塞在
 * pthread_cond_wait 而无法退出。
 */
void pipeline_wake_display_threads(void)
{
    for (int channel_id : g_display_channel_ids)
    {
        pthread_mutex_lock(&g_display_queues[channel_id].mtx);
        pthread_cond_broadcast(&g_display_queues[channel_id].cv);
        pthread_mutex_unlock(&g_display_queues[channel_id].mtx);
    }
}

/**
 * @brief 销毁显示队列互斥量 / 条件变量与通道串行锁。
 *
 * 必须在所有 display_worker_thread、pipeline_dispatch_worker 与 pipeline_logic_worker
 * 退出后调用。
 */
void pipeline_destroy_display_queues(void)
{
    for (int channel_id : g_display_channel_ids)
    {
        pthread_mutex_destroy(&g_display_queues[channel_id].mtx);
        pthread_cond_destroy(&g_display_queues[channel_id].cv);
        g_display_queues[channel_id].pool.deinit(); /* 释放三槽帧缓冲 */
    }
    g_display_channel_ids.clear();
    for (int channel_id = 0; channel_id < MAX_CHANNEL_NUM; ++channel_id)
    {
        TraditionalLogicQueue &queue = g_traditional_logic_queues[channel_id];
        queue.task = TraditionalLogicTask{};
        queue.has_task = false;
        pthread_mutex_destroy(&queue.mtx);
        pthread_cond_destroy(&queue.cv);
    }
    g_logic_channel_ids.clear();
    if (g_process_mutexes_initialized)
    {
        for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
            pthread_mutex_destroy(&g_process_mtx[i]);
        g_process_mutexes_initialized = false;
    }
}

/*======================== 通道热插拔 / 断流重连 ========================*/

void pipeline_channel_offline(int chnId)
{
    if (!app_ctrl_has_channel(chnId))
        return;

    /* 先丢弃未消费的旧帧。已被 worker 取走的帧会在 g_process_mtx 上与
     * 下方状态复位串行，不会在复位之后反向覆盖状态。 */
    TraditionalLogicQueue &logic_queue = g_traditional_logic_queues[chnId];
    pthread_mutex_lock(&logic_queue.mtx);
    logic_queue.task = TraditionalLogicTask{};
    logic_queue.has_task = false;
    pthread_mutex_unlock(&logic_queue.mtx);

    /* 若告警录像仍在等待 post 窗口，断流/本地文件结束即以最后可用帧截断。 */
    event_video_recorder_channel_offline(chnId);

    const auto runtime = app_ctrl_get_runtime_snapshot();
    const ChannelConfig *channel_config = app_ctrl_runtime_channel_config(runtime, chnId);
    ChannelState &ch = g_pCtrl->channels_state[chnId];
    uint64_t ts = 0;
    {
        /* 与逐帧 logic 同锁：保证 ctx->state 不会在 logic 执行中被 reset。 */
        pthread_mutex_lock(&g_process_mtx[chnId]);
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        ch.online_state = CH_OFFLINE;
        ch.offline_ts_ms = steady_now_ms();
        ts = ch.offline_ts_ms;
        /* 清空全部逻辑状态：断线/换源后旧变量不应残留 */
        ch.last_results.clear();
        ch.draw_cmds.clear();
        ch.logic_state.reset();
        ch.logic_outputs = empty_logic_output_snapshot();
        ch.last_lazy_frame.reset();
        ch.logic_display_frame.release();
        ch.logic_frame_id = 0;
        ch.last_logic_ts_ms = ts;
        ch.published_frame_seq = 0;
        const int effective_infer =
            channel_config && config_utils::is_channel_infer_enabled(*channel_config) && ch.infer_runtime_enable;
        ch.commit_publication(runtime, ts, 0, 0, effective_infer);
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
        pthread_mutex_unlock(&g_process_mtx[chnId]);
    }
    feed_stats_reset(chnId); /* 重置 FPS 节流计时，避免重连后偏移错乱 */
    /* reset 接口内部持 g_process_mtx，与 dispatch 的 tracker->update() 互斥。
     * 此处 chn_mtx 已释放，不会形成锁顺序反转。 */
    pipeline_reset_tracker_ids(chnId);
    printf("[Pipeline] ch%d went OFFLINE at %llums (logic_state reset)\n", chnId, (unsigned long long)ts);
}

void pipeline_channel_online(int chnId)
{
    if (!app_ctrl_has_channel(chnId))
        return;

    TraditionalLogicQueue &logic_queue = g_traditional_logic_queues[chnId];
    pthread_mutex_lock(&logic_queue.mtx);
    logic_queue.task = TraditionalLogicTask{};
    logic_queue.has_task = false;
    pthread_mutex_unlock(&logic_queue.mtx);

    const auto runtime = app_ctrl_get_runtime_snapshot();
    const ChannelConfig *channel_config = app_ctrl_runtime_channel_config(runtime, chnId);
    ChannelState &ch = g_pCtrl->channels_state[chnId];
    uint64_t ts = 0;
    {
        pthread_mutex_lock(&g_process_mtx[chnId]);
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        ch.online_state = CH_ONLINE;
        ch.online_ts_ms = steady_now_ms();
        ts = ch.online_ts_ms;
        /* 清空全部逻辑状态：重连/换源后从全新状态开始 */
        ch.last_results.clear();
        ch.draw_cmds.clear();
        ch.logic_state.reset();
        ch.logic_outputs = empty_logic_output_snapshot();
        ch.last_lazy_frame.reset();
        ch.logic_display_frame.release();
        ch.logic_frame_id = 0;
        ch.last_logic_ts_ms = ts;
        ch.published_frame_seq = 0;
        const int effective_infer =
            channel_config && config_utils::is_channel_infer_enabled(*channel_config) && ch.infer_runtime_enable;
        ch.commit_publication(runtime, ts, 0, 0, effective_infer);
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
        pthread_mutex_unlock(&g_process_mtx[chnId]);
    }
    feed_stats_reset(chnId);
    /* 同 offline：reset 接口内部与 tracker->update() 串行。 */
    pipeline_reset_tracker_ids(chnId);
    printf("[Pipeline] ch%d came ONLINE at %llums (logic_state reset)\n", chnId, (unsigned long long)ts);
}

/*======================== 线程数量 / 通道号查询（供 main 创建线程）========================*/

/**
 * @brief 返回需要创建的显示线程数量（= 活跃通道数）。
 */
int pipeline_get_display_thread_count(void)
{
    return static_cast<int>(g_display_channel_ids.size());
}

/**
 * @brief 返回第 idx 个显示线程对应的通道号（一对一映射）。
 */
int pipeline_get_display_chn_id(int idx)
{
    return idx >= 0 && idx < static_cast<int>(g_display_channel_ids.size()) ? g_display_channel_ids[idx] : -1;
}

/**
 * @brief 返回需要创建的推理分发线程数量（仅统计启用推理的通道）。
 */
int pipeline_get_dispatch_thread_count(void)
{
    int count = 0;
    auto runtime = app_ctrl_get_runtime_snapshot();
    if (!runtime)
        return 0;
    const int n = static_cast<int>(runtime->config.channels.size());
    for (int i = 0; i < n && i < MAX_CHANNEL_NUM; ++i)
    {
        const ChannelConfig &ch = runtime->config.channels[i];
        if (!ch.enable)
            continue;
        if (!config_utils::is_channel_infer_enabled(ch))
            continue;
        count++;
    }
    return count;
}

/**
 * @brief 返回第 idx 个推理分发线程对应的通道号。
 *        若 idx 越界返回 -1。
 */
int pipeline_get_dispatch_chn_id(int idx)
{
    int count = 0;
    auto runtime = app_ctrl_get_runtime_snapshot();
    if (!runtime)
        return -1;
    const int n = static_cast<int>(runtime->config.channels.size());
    for (int i = 0; i < n && i < MAX_CHANNEL_NUM; ++i)
    {
        const ChannelConfig &ch = runtime->config.channels[i];
        if (!ch.enable)
            continue;
        if (!config_utils::is_channel_infer_enabled(ch))
            continue;
        if (count == idx)
            return ch.id;
        count++;
    }
    return -1;
}

/**
 * @brief 返回传统逻辑 worker 数量。worker 长期阻塞在条件变量上，无任务时不占 CPU。
 */
int pipeline_get_logic_thread_count(void)
{
    return static_cast<int>(g_logic_channel_ids.size());
}

int pipeline_get_logic_chn_id(int idx)
{
    return idx >= 0 && idx < static_cast<int>(g_logic_channel_ids.size()) ? g_logic_channel_ids[idx] : -1;
}
