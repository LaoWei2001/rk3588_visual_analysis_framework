/**
 * @file channel_pipeline.cpp
 * @brief 通道结果处理管线
 *
 * 职责:
 *   - 跟踪器管理 (SORT, 每通道独立实例)
 *   - invoke_channel_logic(): 构造 ChannelContext, 调用已注册的 logic 函数,
 *     将结果和绘制指令写回共享状态 (持 chn_mtx 原子完成)
 *   - process_channel_results(): ROI 缩放 + tracker + invoke_channel_logic
 *     两条路径: 推理通道 (new_results 非空) / 非推理直通通道
 */

#include <cstdio>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <pthread.h>
#include <opencv2/opencv.hpp>

#include "analyzer_internal.h"
#include "analyzer.h"
#include "tracker.h"
#include "../control/channel_control.h"
#include "../core/image_utils.h"
#include "logic/core/channel_logic.h"

/*======================== 跟踪器 (每通道一个实例) ========================*/

static std::unique_ptr<Tracker> g_trackers[MAX_CHANNEL_NUM];

static Tracker *get_tracker(int chnId, const ChannelConfig *ch_cfg)
{
    if (!ch_cfg || !ch_cfg->tracker_enable)
    {
        g_trackers[chnId].reset();
        return nullptr;
    }
    if (!g_trackers[chnId])
        g_trackers[chnId] = std::make_unique<Tracker>(
            ch_cfg->tracker_iou_thresh, ch_cfg->tracker_max_miss, ch_cfg->tracker_min_hits);
    return g_trackers[chnId].get();
}

static void update_tracker_locked(int chnId, const ChannelConfig &next,
                                  const ChannelConfig *previous, bool force_reset)
{
    if (!next.tracker_enable)
    {
        g_trackers[chnId].reset();
        return;
    }

    const bool min_hits_changed = previous &&
        previous->tracker_min_hits != next.tracker_min_hits;
    if (!g_trackers[chnId] || min_hits_changed)
    {
        g_trackers[chnId] = std::make_unique<Tracker>(
            next.tracker_iou_thresh, next.tracker_max_miss, next.tracker_min_hits);
    }
    else
    {
        g_trackers[chnId]->setTrackerIoUThresh(next.tracker_iou_thresh);
        g_trackers[chnId]->setTrackerMaxMiss(next.tracker_max_miss);
        if (force_reset)
            g_trackers[chnId]->reset();
    }
}

void trackers_init(void)
{
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        g_trackers[i].reset();
}

void trackers_deinit(void)
{
    for (int i = 0; i < MAX_CHANNEL_NUM; ++i)
        g_trackers[i].reset();
}

/* 公开接口: 供 config_monitor 热重载时更新跟踪器参数 */
void analyzer_update_tracker(int chnId, const ChannelConfig *ch)
{
    if (!ch || chnId < 0 || chnId >= MAX_CHANNEL_NUM) return;
    pthread_mutex_lock(&g_process_mtx[chnId]);
    update_tracker_locked(chnId, *ch, nullptr, false);
    pthread_mutex_unlock(&g_process_mtx[chnId]);
}

void analyzer_reset_tracker_ids(int chnId)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM) return;
    pthread_mutex_lock(&g_process_mtx[chnId]);
    if (g_trackers[chnId])
    {
        g_trackers[chnId]->reset();
        printf("[ChannelPipeline] tracker state reset for ch%d\n", chnId);
    }
    pthread_mutex_unlock(&g_process_mtx[chnId]);
}

bool analyzer_publish_runtime_snapshot(
    const AppConfig &config, uint64_t generation,
    const std::vector<int> &logic_changed_channels,
    const std::vector<int> &tracker_reset_channels)
{
    if (!g_pCtrl) return false;

    auto next = app_ctrl_build_runtime_snapshot(
        config, g_pCtrl->inputW, g_pCtrl->inputH, generation);
    if (!next) return false;
    auto previous = app_ctrl_get_runtime_snapshot();

    bool logic_changed[MAX_CHANNEL_NUM]{};
    bool tracker_reset[MAX_CHANNEL_NUM]{};
    for (int channel_id : logic_changed_channels)
        if (channel_id >= 0 && channel_id < MAX_CHANNEL_NUM)
            logic_changed[channel_id] = true;
    for (int channel_id : tracker_reset_channels)
        if (channel_id >= 0 && channel_id < MAX_CHANNEL_NUM)
            tracker_reset[channel_id] = true;

    std::vector<int> channel_ids;
    channel_ids.reserve(config.channels.size());
    for (const auto &channel : config.channels)
        if (channel.id >= 0 && channel.id < MAX_CHANNEL_NUM)
            channel_ids.push_back(channel.id);
    std::sort(channel_ids.begin(), channel_ids.end());
    channel_ids.erase(std::unique(channel_ids.begin(), channel_ids.end()), channel_ids.end());

    /* 固定按 channel_id 加锁，保证多通道发布不会产生锁顺序反转。
     * 快照已经在锁外完成构造，暂停窗口只包含指针交换与少量状态复位。 */
    for (int channel_id : channel_ids)
        pthread_mutex_lock(&g_process_mtx[channel_id]);

    app_ctrl_store_runtime_snapshot(next);

    for (int channel_id : channel_ids)
    {
        const ChannelConfig *next_channel =
            app_ctrl_runtime_channel_config(next, channel_id);
        const ChannelConfig *previous_channel =
            app_ctrl_runtime_channel_config(previous, channel_id);
        if (!next_channel) continue;

        const bool reset_tracker = tracker_reset[channel_id] || logic_changed[channel_id];
        update_tracker_locked(channel_id, *next_channel, previous_channel, reset_tracker);

        if (logic_changed[channel_id])
        {
            pthread_mutex_lock(&g_pCtrl->chn_mtx[channel_id]);
            ChannelState &state = g_pCtrl->channels_state[channel_id];
            state.logic_state.reset();
            state.last_results.clear();
            state.draw_cmds.clear();
            state.logic_display_frame.release();
            state.logic_frame_id = 0;
            state.result_frame_seq = 0;
            state.last_logic_ts_ms = steady_now_ms();
            pthread_mutex_unlock(&g_pCtrl->chn_mtx[channel_id]);
        }
    }

    for (auto it = channel_ids.rbegin(); it != channel_ids.rend(); ++it)
        pthread_mutex_unlock(&g_process_mtx[*it]);

    for (int channel_id : logic_changed_channels)
        feed_stats_reset(channel_id);

    printf("[ConfigMonitor] Runtime snapshot generation %llu published (%zu channels)\n",
           static_cast<unsigned long long>(generation), channel_ids.size());
    return true;
}

/*======================== 原始分辨率帧的惰性访问 ========================*/

struct SourceFrameAccess
{
    const ChannelRawFrame *raw = nullptr;
    cv::Mat bgr;
    bool attempted = false;
};

static const cv::Mat *get_source_frame_bgr(void *opaque)
{
    SourceFrameAccess *access = static_cast<SourceFrameAccess *>(opaque);
    if (!access || !access->raw) return nullptr;
    if (access->attempted) return access->bgr.empty() ? nullptr : &access->bgr;
    access->attempted = true;

    const ChannelRawFrame &raw = *access->raw;
    if (!raw.source_data || raw.width <= 0 || raw.height <= 0 ||
        raw.source_hstride <= 0 || raw.source_vstride <= 0)
        return nullptr;

    if (!raw_to_bgr_mat(raw.source_data, raw.width, raw.height,
                        raw.source_hstride, raw.source_vstride,
                        raw.source_format, access->bgr))
        return nullptr;
    return access->bgr.empty() ? nullptr : &access->bgr;
}

/*======================== invoke_channel_logic ========================*/
/**
 * @brief 调用通道 logic 函数并将结果原子写回共享状态。
 *
 * 从原 process_channel_results 内的 lambda 提升为具名函数，方便调试
 * (调用栈中可见函数名) 并允许将来单独测试 logic 调用路径。
 *
 * 持 chn_mtx[chnId] 的时间窗口（已优化）：
 *   fn(&ctx) 在锁外运行；仅写回 last_logic_frame/last_results/draw_cmds 时短暂持锁。
 * 这使 get_channel_snapshot() 等待时间从"logic 执行时长"降至"赋值时长"（μs 级）。
 *
 * @param chnId          通道号
 * @param frame_for_logic 与 current_results 严格同帧的 640 BGR 图
 * @param current_results 当帧检测结果（tracker 已更新 track_id）
 * @param frame_id        帧序号（用于写 result_frame_seq）
 * @param timestamp_ms    帧时间戳（毫秒）
 * @param dt_ms           距上一帧的时间间隔（毫秒），供 logic 做积分
 * @param infer_enabled   本通道是否开启推理（透传给 ctx）
 * @param raw_frame       当前同步解码源帧；异步推理路径中不含有效 source_data
 */
static void invoke_channel_logic(int chnId,
                                  const cv::Mat &frame_for_logic,
                                  std::vector<AlgoResult> &current_results,
                                  int64_t frame_id,
                                  uint64_t timestamp_ms,
                                  float dt_ms,
                                  int infer_enabled,
                                  const ChannelRawFrame *raw_frame,
                                  const std::shared_ptr<const AppRuntimeSnapshot> &runtime)
{
    const ChannelConfig *channel_config =
        app_ctrl_runtime_channel_config(runtime, chnId);
    const std::vector<RoiZone> *runtime_rois =
        app_ctrl_runtime_channel_rois(runtime, chnId);
    const LogicParameterSet *runtime_logic_parameters =
        app_ctrl_runtime_logic_parameters(runtime, chnId);
    if (!channel_config || !runtime_rois || !runtime_logic_parameters) return;
    const std::string &logic_name = channel_config->logic;

    /* 未配置后处理模块：不构造 ChannelContext，也不调用任何业务函数。
     * 仍提交严格同帧的 frame/results，供通用检测框绘制、快照和跨通道读取使用。 */
    if (logic_name.empty())
    {
        ChannelState &ch_state = g_pCtrl->channels_state[chnId];
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        ch_state.last_logic_frame  = frame_for_logic;
        ch_state.logic_state.reset();
        ch_state.last_results      = current_results;
        ch_state.result_frame_seq  = frame_id;
        ch_state.last_result_ts_ms = steady_now_ms();
        ch_state.draw_cmds.clear();
        ch_state.logic_display_frame.release();
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
        return;
    }

    ChannelLogicFunc fn = channel_logic_get(logic_name.c_str());
    if (!fn) return;

    ChannelState &ch_state = g_pCtrl->channels_state[chnId];
    std::shared_ptr<void> logic_state;

    /* 构造 ChannelContext（栈上，logic 函数只在本次调用内使用）*/
    ChannelContext ctx;
    ctx.chnId         = chnId;
    ctx.frame         = &frame_for_logic;
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    ctx.src_width     = ch_state.src_w_now;   /* 原始视频分辨率(解码源帧尺寸, 如 1920×1080) */
    ctx.src_height    = ch_state.src_h_now;
    ctx.disp_fps      = ch_state.disp_fps;
    logic_state       = ch_state.logic_state;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    ctx.frame_id      = frame_id;
    ctx.timestamp_ms  = timestamp_ms;
    /* 墙上时钟(epoch ms): RTSP/USB/文件 三源统一在此盖一次, logic 读 ctx->unix_ms / time_hms()
     * 即得本帧真实时间。这是"处理本帧的时刻", 与采集相差一个管线延迟(对 HH:MM:SS 显示无感)。 */
    ctx.unix_ms       = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    ctx.dt_ms         = dt_ms;
    ctx.results       = &current_results;
    ctx.config        = channel_config;
    ctx.logic_parameters = runtime_logic_parameters;
    /* 多 ROI: ctx.rois = 本通道全部区域; ctx.roi = 第一个区域(兼容老逻辑, 无区域时为 nullptr)。
     * 顶点已是模型坐标系(归一化加载时即是; 旧像素格式由下方 process_channel_results 缩放后填入)。*/
    ctx.rois          = runtime_rois;
    ctx.roi           = runtime_rois->empty() ? nullptr : &(*runtime_rois)[0].polygon;
    ctx.state         = &logic_state;
    ctx.infer_enabled = infer_enabled;
    ctx.infer_fps     = algorithm_get_infer_fps(chnId);

    /* 原始帧只绑定短生命周期的惰性转换器。传统 CV 通道与 videoOutHandle 同步执行，
     * raw_frame.source_data 在 fn(&ctx) 返回前有效；异步推理路径没有该借用视图。 */
    SourceFrameAccess source_frame_access;
    source_frame_access.raw = raw_frame;
    if (raw_frame && raw_frame->source_data)
    {
        ctx.source_frame_getter = get_source_frame_bgr;
        ctx.source_frame_opaque = &source_frame_access;
    }

    /* logic 在 chn_mtx 外运行。runtime shared_ptr 保证 config/ROI 在本帧全程有效；
     * ctx.state 由外层 g_process_mtx 保护，热更新只能在本帧 logic 返回后切换。 */
    std::vector<DrawCommand> draw_cmds;
    ctx.draw_cmds = &draw_cmds;
    /* 显示画布(可选): logic 调 ctx->display_canvas() 才会启用并克隆，不调则零开销 */
    cv::Mat canvas_buf;
    bool    show_canvas = false;
    ctx.canvas      = &canvas_buf;
    ctx.show_canvas = &show_canvas;
    std::vector<ChannelAction> pending_actions;
    channel_control_take(chnId, pending_actions);
    if (!pending_actions.empty())
    {
        ChannelActionFunc action_fn = channel_logic_action_get(logic_name.c_str());
        for (const auto &action : pending_actions)
        {
            if (action.logic_name != logic_name)
            {
                printf("[ChannelAction][ch%02d][%s] drop action=%s request=%s (queued for %s)\n",
                       chnId, logic_name.c_str(), action.name.c_str(),
                       action.request_id.c_str(), action.logic_name.c_str());
                continue;
            }
            if (!action_fn)
            {
                printf("[ChannelAction][ch%02d][%s] no handler for action=%s request=%s\n",
                       chnId, logic_name.c_str(), action.name.c_str(), action.request_id.c_str());
                continue;
            }
            ChannelActionResult result = action_fn(&ctx, &action);
            printf("[ChannelAction][ch%02d][%s] action=%s request=%s handled=%d msg=%s\n",
                   chnId, logic_name.c_str(), action.name.c_str(), action.request_id.c_str(),
                   result.handled ? 1 : 0, result.message.c_str());
        }
    }
    fn(&ctx);


    /* 原子写回共享状态：get_channel_snapshot() 在同一把锁内读出，三者必定同帧。*/
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        ch_state.last_logic_frame  = frame_for_logic;
        ch_state.logic_state       = std::move(logic_state);
        ch_state.last_results      = current_results;
        ch_state.result_frame_seq  = frame_id;
        ch_state.last_result_ts_ms = steady_now_ms();
        ch_state.draw_cmds         = std::move(draw_cmds);
        /* logic 拦截了整帧 → 存为本通道显示底图；否则清掉，显示回到实时采集帧 */
        if (show_canvas && !canvas_buf.empty()) {
            ch_state.logic_display_frame = std::move(canvas_buf);
            ch_state.logic_display_ts_ms = steady_now_ms();
        } else {
            ch_state.logic_display_frame.release();
        }
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    }
}

/*======================== process_channel_results ========================*/
/**
 * @brief 每帧结果处理入口：ROI 缩放缓存 → tracker → invoke_channel_logic。
 *
 * 两条路径：
 *   new_results == nullptr  → 非推理通道；配置了 logic 时用空结果执行，否则只提交帧
 *   new_results != nullptr  → 推理通道，先过 tracker，再按需执行 logic 并提交结果
 *
 * 调用者需在 g_process_mtx[chnId] 保护下调用，防止两条路径并发
 * （videoOutHandle 非推理直通 / dispatch_worker 推理完成通知 可能同时触发）。
 */
std::vector<AlgoResult> process_channel_results(
    int chnId,
    const ChannelRawFrame &raw_frame,
    std::vector<AlgoResult> *new_results,
    cv::Mat                 *infer_frame,
    int64_t                  result_frame_id)
{
    if (!g_pCtrl) return {};
    ChannelState &ch_state = g_pCtrl->channels_state[chnId];
    const auto runtime = app_ctrl_get_runtime_snapshot();
    const ChannelConfig *channel_config =
        app_ctrl_runtime_channel_config(runtime, chnId);
    if (!channel_config) return {};

    /* 存储最新解码帧（RGA 失败时作兜底）*/
    if (!raw_frame.model_input_mat.empty())
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        if (raw_frame.model_input_mat.data != ch_state.last_frame.data)
            ch_state.last_frame = raw_frame.model_input_mat;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    }

    const uint64_t now_ms = steady_now_ms();

    int infer_enabled = 0;
    infer_enabled = config_utils::is_channel_infer_enabled(*channel_config) ? 1 : 0;

    /* ---- 路径1：非推理通道 / 无结果直通 ---- */
    if (!infer_enabled || !new_results)
    {
        int64_t logic_frame_id = 0;
        float dt_ms = 0.0f;
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        logic_frame_id = ++ch_state.logic_frame_id;
        if (logic_frame_id > 1)
            dt_ms = static_cast<float>(now_ms - ch_state.last_logic_ts_ms);
        ch_state.last_logic_ts_ms = now_ms;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);

        std::vector<AlgoResult> empty_results;
        /* 防御：RGA 转换失败且无历史帧时 model_input_mat 为空。
         * 跳过 logic 调用，避免 logic 对空 cv::Mat 做矩阵运算崩溃。 */
        if (!raw_frame.model_input_mat.empty())
        {
            invoke_channel_logic(chnId, raw_frame.model_input_mat,
                                 empty_results, logic_frame_id,
                                 now_ms, dt_ms, infer_enabled, &raw_frame, runtime);
        }
        return empty_results;
    }

    /* ---- 路径2：推理通道 ---- */
    std::vector<AlgoResult> results = *new_results;

    float dt_ms = 0.0f;
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    const int64_t logic_frame_id = ++ch_state.logic_frame_id;
    if (logic_frame_id > 1)
        dt_ms = static_cast<float>(now_ms - ch_state.last_logic_ts_ms);
    ch_state.last_logic_ts_ms = now_ms;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);

    if (Tracker *tracker = get_tracker(chnId, channel_config))
        tracker->update(results);

    const int64_t  frame_seq = result_frame_id;
    const uint64_t frame_ts  = !results.empty() ? results.front().timestamp_ms : now_ms;

    const cv::Mat &frame_for_logic = (infer_frame && !infer_frame->empty())
                                         ? *infer_frame
                                         : raw_frame.model_input_mat;

    std::vector<AlgoResult> out = std::move(results);
    for (auto &result : out) result.chn_id = chnId;
    /* 防御：若 infer_frame 与 last_frame 均为空（极少见），跳过 logic 调用。*/
    if (frame_for_logic.empty())
        return out;
    invoke_channel_logic(chnId, frame_for_logic, out, frame_seq,
                         frame_ts, dt_ms, infer_enabled, &raw_frame, runtime);
    return out;
}
