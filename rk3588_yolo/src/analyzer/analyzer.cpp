/**
 * @file analyzer.cpp
 * @brief 分析器模块 — 共享状态定义 + 生命周期管理
 *
 * 本文件仅负责:
 *   1. 定义跨文件共享的 extern 变量
 *        g_disp_queues   — 显示单槽队列（frame_inlet + display_pipeline 共用）
 *        g_dispatch_running — dispatch_worker_thread 退出信号
 *        g_process_mtx   — 同通道 process_channel_results 串行锁
 *   2. load_roi_zones_from_config()  — 从 ChannelConfig 加载 ROI 多边形到各通道状态
 *   3. analyzer_init / analyzer_deinit — 模块启动 / 关闭
 *   4. analyzer_wake_display_threads / analyzer_destroy_display_queues
 *        供 main 在线程退出前调用
 *   5. analyzer_get_display/dispatch_thread_count/id
 *        供 main 决定创建多少线程、各线程对应哪个通道
 *
 * 各路径实现已拆分到独立文件（均通过 analyzer_internal.h 共享状态）:
 *   frame_inlet.cpp       — videoOutHandle + FPS 节流 + RGA 转换 + 统计
 *   channel_pipeline.cpp  — 跟踪器 + process_channel_results + invoke_channel_logic
 *   result_dispatch.cpp   — dispatch_worker_thread
 *   display_pipeline.cpp  — display_worker_thread
 */

#include <cstdio>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <cmath>
#include <pthread.h>

#include "system.h"
#include "analyzer.h"
#include "analyzer_internal.h" /* DispTask/DispQueue 定义、extern 声明、时间辅助 */
#include "algoProcess.h"
#include "../logic/channel_logic.h"
#include "../logic/global_logic.h"

/*======================== 共享 extern 变量定义 ========================*/
/* 声明在 analyzer_internal.h（extern），此处给出唯一定义。 */

DispQueue g_disp_queues[MAX_CHANNEL_NUM];
volatile int g_dispatch_running = 0;
pthread_mutex_t g_process_mtx[MAX_CHANNEL_NUM];

/*======================== ROI 加载 ========================*/

/**
 * @brief 从 AppConfig.channels[i].roi_zones/roi_polygon 加载 ROI 区域到 ChannelState。
 *
 * 坐标全部为归一化 (0~1), 直接 × 模型尺寸 → 模型坐标系。不再依赖外部 roi_zones.json 文件。
 */
void load_roi_zones_from_config(void)
{
    const int mw = g_pCtrl->inputW, mh = g_pCtrl->inputH;
    if (mw <= 0 || mh <= 0) return;

    const int n = app_ctrl_get_chn_nums();
    for (int ch = 0; ch < n && ch < MAX_CHANNEL_NUM; ++ch)
    {
        const auto &chCfg = g_pCtrl->config.channels[ch];
        auto &state = g_pCtrl->channels_state[ch];
        state.roi_zones.clear();
        state.roi_zones_raw.clear();
        state.roi_model_space = true;   /* config.json 坐标均为归一化 */
        state.last_src_w = 0;
        state.last_src_h = 0;

        auto add_zone = [&](const std::string &name,
                            const std::vector<std::pair<double, double>> &poly) {
            if (poly.size() < 3) return;
            RoiZone zone;
            zone.name = name;
            for (const auto &pt : poly)
                zone.polygon.emplace_back((int)(pt.first * mw + 0.5),
                                          (int)(pt.second * mh + 0.5));
            state.roi_zones.push_back(std::move(zone));
        };

        /* 优先 roi_zones (多区域, 各有名称) */
        if (!chCfg.roi_zones.empty())
        {
            for (const auto &zc : chCfg.roi_zones)
                add_zone(zc.name, zc.polygon);
        }
        /* 回退 roi_polygon (单区域, 无名, 兼容旧配置) */
        else if (!chCfg.roi_polygon.empty())
        {
            add_zone(std::string(), chCfg.roi_polygon);
        }
    }
}

/*======================== 初始化 / 反初始化 ========================*/

int analyzer_init(void)
{
    /* 跟踪器全部清零（各通道首次调用 process_channel_results 时惰性创建）*/
    trackers_init();

    if (algorithm_init(g_pCtrl->config) != 0)
        return -1;
    g_pCtrl->inputW = algorithm_get_input_w();
    g_pCtrl->inputH = algorithm_get_input_h();

    load_roi_zones_from_config();
    global_logic_start_all(g_pCtrl->config.global_logics);

    /* 设置各通道初始 logic 名称（热重载时由 config_monitor 更新）*/
    const int num_chn = app_ctrl_get_chn_nums();
    for (int i = 0; i < num_chn && i < MAX_CHANNEL_NUM; ++i)
        g_pCtrl->channels_state[i].logic_name = g_pCtrl->config.channels[i].logic;

    /* 启用分发线程运行标志 */
    g_dispatch_running = 1;

    /* 初始化显示队列互斥量 / 条件变量 / 帧池 */
    for (int i = 0; i < num_chn && i < MAX_CHANNEL_NUM; ++i)
    {
        pthread_mutex_init(&g_disp_queues[i].mtx, nullptr);
        pthread_cond_init(&g_disp_queues[i].cv, nullptr);
        g_disp_queues[i].has_task = 0;
        g_disp_queues[i].pool.init(); /* 预分配三槽帧缓冲（≈9 MB/通道，NV12 1080p）*/
    }

    /* 初始化通道串行锁（所有通道，不仅是活跃通道）*/
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        pthread_mutex_init(&g_process_mtx[i], nullptr);

    printf("[Analyzer] initialized. Thread creation managed by main.\n");
    return 0;
}

void analyzer_deinit(void)
{
    /* 通知 dispatch_worker_thread 退出（algorithm_deinit 会 signal 所有等待的 worker）*/
    g_dispatch_running = 0;
    algorithm_deinit();

    global_logic_stop_all();
    trackers_deinit();
}

/*======================== 显示线程辅助（供 main 调用）========================*/

/**
 * @brief 广播所有显示队列条件变量，使阻塞中的 display_worker_thread 能检测到退出信号。
 *
 * main 在设置 g_pCtrl->isRunning = 0 之后调用，防止线程永久阻塞在
 * pthread_cond_wait 而无法退出。
 */
void analyzer_wake_display_threads(void)
{
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        pthread_cond_broadcast(&g_disp_queues[i].cv);
}

/**
 * @brief 销毁显示队列互斥量 / 条件变量与通道串行锁。
 *
 * 必须在所有 display_worker_thread 与 dispatch_worker_thread 退出后调用。
 */
void analyzer_destroy_display_queues(void)
{
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
    {
        pthread_mutex_destroy(&g_disp_queues[i].mtx);
        pthread_cond_destroy(&g_disp_queues[i].cv);
        g_disp_queues[i].pool.deinit(); /* 释放三槽帧缓冲 */
    }
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        pthread_mutex_destroy(&g_process_mtx[i]);
}

/*======================== 通道热插拔 / 断流重连 ========================*/

void analyzer_channel_offline(int chnId)
{
    if (!g_pCtrl || chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return;

    ChannelState &ch = g_pCtrl->channels_state[chnId];
    uint64_t ts = 0;
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        ch.online_state = CH_OFFLINE;
        ch.offline_ts_ms = steady_now_ms();
        ts = ch.offline_ts_ms;
        /* 清空全部逻辑状态：断线/换源后旧变量不应残留 */
        ch.last_results.clear();
        ch.draw_cmds.clear();
        ch.logic_state.reset();
        ch.logic_frame_id = 0;
        ch.last_logic_ts_ms = ts;
        ch.result_frame_seq = 0;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    }
    feed_stats_reset(chnId);           /* 重置 FPS 节流计时，避免重连后偏移错乱 */
    /* 持 g_process_mtx 后再 reset tracker，与 dispatch_worker_thread 中的
     * tracker->update() 互斥，防止两线程并发访问 KalmanFilter 矩阵导致
     * "One or more matrix operands are empty" 崩溃。
     * 加锁前 chn_mtx 已释放，与 process_channel_results 的加锁顺序
     * (g_process_mtx→chn_mtx) 不形成死锁。 */
    pthread_mutex_lock(&g_process_mtx[chnId]);
    analyzer_reset_tracker_ids(chnId);
    pthread_mutex_unlock(&g_process_mtx[chnId]);
    printf("[Analyzer] ch%d went OFFLINE at %llums (logic_state reset)\n",
           chnId, (unsigned long long)ts);
}

void analyzer_channel_online(int chnId)
{
    if (!g_pCtrl || chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return;

    ChannelState &ch = g_pCtrl->channels_state[chnId];
    uint64_t ts = 0;
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        ch.online_state = CH_ONLINE;
        ch.online_ts_ms = steady_now_ms();
        ts = ch.online_ts_ms;
        /* 清空全部逻辑状态：重连/换源后从全新状态开始 */
        ch.last_results.clear();
        ch.draw_cmds.clear();
        ch.logic_state.reset();
        ch.logic_frame_id = 0;
        ch.last_logic_ts_ms = ts;
        ch.result_frame_seq = 0;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    }
    feed_stats_reset(chnId);
    /* 同 offline：持 g_process_mtx 后再 reset tracker，防止与
     * dispatch_worker_thread 中正在执行的 tracker->update() 并发。 */
    pthread_mutex_lock(&g_process_mtx[chnId]);
    analyzer_reset_tracker_ids(chnId);
    pthread_mutex_unlock(&g_process_mtx[chnId]);
    printf("[Analyzer] ch%d came ONLINE at %llums (logic_state reset)\n",
           chnId, (unsigned long long)ts);
}

int analyzer_is_channel_online(int chnId)
{
    if (!g_pCtrl || chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return 0;
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    const ChannelOnlineState s = g_pCtrl->channels_state[chnId].online_state;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    return (s != CH_OFFLINE) ? 1 : 0;
}

ChannelHealth analyzer_get_channel_health(int chnId, int stale_ms, int dead_ms)
{
    if (!g_pCtrl || chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return CH_HEALTH_DEAD;
    /* last_infer_ts_ms 由 videoOutHandle 写，仅当 will_infer=1 时更新，无锁读可接受 */
    const uint64_t last_ts = g_pCtrl->channels_state[chnId].last_infer_ts_ms;
    if (last_ts == 0)
        return CH_HEALTH_DEAD; /* 从未推理过 */
    const int64_t age_ms = (int64_t)(steady_now_ms() - last_ts);
    if (age_ms >= dead_ms)
        return CH_HEALTH_DEAD;
    if (age_ms >= stale_ms)
        return CH_HEALTH_STALE;
    return CH_HEALTH_HEALTHY;
}

/*======================== 线程数量 / 通道号查询（供 main 创建线程）========================*/

/**
 * @brief 返回需要创建的显示线程数量（= 活跃通道数）。
 */
int analyzer_get_display_thread_count(void)
{
    const int n = app_ctrl_get_chn_nums();
    return (n > 0 && n <= MAX_CHANNEL_NUM) ? n : 0;
}

/**
 * @brief 返回第 idx 个显示线程对应的通道号（一对一映射）。
 */
int analyzer_get_display_chn_id(int idx)
{
    return idx;
}

/**
 * @brief 返回需要创建的推理分发线程数量（仅统计启用推理的通道）。
 */
int analyzer_get_dispatch_thread_count(void)
{
    int count = 0;
    const int n = app_ctrl_get_chn_nums();
    for (int i = 0; i < n && i < MAX_CHANNEL_NUM; ++i)
    {
        const ChannelConfig &ch = g_pCtrl->config.channels[i];
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
int analyzer_get_dispatch_chn_id(int idx)
{
    int count = 0;
    const int n = app_ctrl_get_chn_nums();
    for (int i = 0; i < n && i < MAX_CHANNEL_NUM; ++i)
    {
        const ChannelConfig &ch = g_pCtrl->config.channels[i];
        if (!ch.enable)
            continue;
        if (!config_utils::is_channel_infer_enabled(ch))
            continue;
        if (count == idx)
            return i;
        count++;
    }
    return -1;
}
