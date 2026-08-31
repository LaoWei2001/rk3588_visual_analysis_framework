/**
 * @file channel_pipeline.cpp
 * @brief 通道结果处理管线
 *
 * 职责:
 *   - 跟踪器管理 (SORT, 每通道独立实例)
 *   - invoke_channel_logic(): 构造 ChannelContext, 调用已注册的 logic 函数,
 *     将结果和绘制指令写回共享状态 (持 chn_mtx 原子完成)
 *   - process_channel_results(): ROI 缩放 + tracker + invoke_channel_logic
 *     两条路径: 推理通道 (new_results 非空) / 非推理异步 logic worker
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <string>
#include <vector>

#include "control/logic_control.h"
#include "pipeline_runtime.h"
#include "pipeline_internal.h"
#include "frame_transform.h"
#include "logic/core/channel_logic.h"
#include "tracking/tracker.h"

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
        g_trackers[chnId] =
            std::make_unique<Tracker>(ch_cfg->tracker_iou_thresh, ch_cfg->tracker_max_miss, ch_cfg->tracker_min_hits);
    return g_trackers[chnId].get();
}

static void update_tracker_locked(int chnId, const ChannelConfig &next, const ChannelConfig *previous, bool force_reset)
{
    if (!next.tracker_enable)
    {
        g_trackers[chnId].reset();
        return;
    }

    const bool min_hits_changed = previous && previous->tracker_min_hits != next.tracker_min_hits;
    if (!g_trackers[chnId] || min_hits_changed)
    {
        g_trackers[chnId] =
            std::make_unique<Tracker>(next.tracker_iou_thresh, next.tracker_max_miss, next.tracker_min_hits);
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

void pipeline_reset_tracker_ids(int chnId)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return;
    pthread_mutex_lock(&g_process_mtx[chnId]);
    if (g_trackers[chnId])
    {
        g_trackers[chnId]->reset();
        printf("[ChannelPipeline] tracker state reset for ch%d\n", chnId);
    }
    pthread_mutex_unlock(&g_process_mtx[chnId]);
}

bool pipeline_publish_runtime_snapshot(const AppConfig &config, uint64_t generation,
                                       const std::vector<int> &logic_changed_channels,
                                       const std::vector<int> &tracker_reset_channels)
{
    if (!g_pCtrl)
        return false;

    auto next = app_ctrl_build_runtime_snapshot(config, g_pCtrl->inputW, g_pCtrl->inputH, generation);
    if (!next)
        return false;
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
        const ChannelConfig *next_channel = app_ctrl_runtime_channel_config(next, channel_id);
        const ChannelConfig *previous_channel = app_ctrl_runtime_channel_config(previous, channel_id);
        if (!next_channel)
            continue;

        const bool reset_tracker = tracker_reset[channel_id] || logic_changed[channel_id];
        update_tracker_locked(channel_id, *next_channel, previous_channel, reset_tracker);

        if (logic_changed[channel_id])
        {
            pthread_mutex_lock(&g_pCtrl->chn_mtx[channel_id]);
            ChannelState &state = g_pCtrl->channels_state[channel_id];
            state.logic_state.reset();
            state.logic_outputs = empty_logic_output_snapshot();
            state.last_results.clear();
            state.draw_cmds.clear();
            state.last_lazy_frame.reset();
            state.logic_display_frame.release();
            state.logic_frame_id = 0;
            state.published_frame_seq = 0;
            const uint64_t now_ms = steady_now_ms();
            state.last_logic_ts_ms = now_ms;
            const bool configured_infer = config_utils::is_channel_infer_enabled(*next_channel);
            const int effective_infer = configured_infer && state.infer_runtime_enable;
            state.commit_publication(next, now_ms, 0, 0, effective_infer);
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

/*======================== 当前视频帧的统一惰性访问 ========================*/
static const cv::Mat *get_model_frame_bgr(void *opaque)
{
    LazyVideoFrame *frame = static_cast<LazyVideoFrame *>(opaque);
    return frame ? frame->model_frame() : nullptr;
}

static const cv::Mat *get_source_frame_bgr(void *opaque)
{
    LazyVideoFrame *frame = static_cast<LazyVideoFrame *>(opaque);
    return frame ? frame->source_frame() : nullptr;
}

/*======================== invoke_channel_logic ========================*/
/**
 * @brief 调用通道 logic 函数并将结果原子写回共享状态。
 *
 * 从原 process_channel_results 内的 lambda 提升为具名函数，方便调试
 * (调用栈中可见函数名) 并允许将来单独测试 logic 调用路径。
 *
 * 持 chn_mtx[chnId] 的时间窗口（已优化）：
 *   fn(&ctx) 在锁外运行；仅写回 lazy_frame/last_results/draw_cmds 时短暂持锁。
 * 这使快照读取等待时间从"logic 执行时长"降至"赋值时长"（μs 级）。
 *
 * @param chnId          通道号
 * @param current_results 当帧检测结果（tracker 已更新 track_id）
 * @param frame_id        帧序号（用于写 published_frame_seq）
 * @param timestamp_ms    帧时间戳（毫秒）
 * @param unix_ms         帧 Unix epoch 时间戳（毫秒）
 * @param dt_ms           距上一帧的时间间隔（毫秒），供 logic 做积分
 * @param infer_enabled   本通道是否开启推理（透传给 ctx）
 * @param raw_frame       当前同步解码源帧；异步推理路径中不含有效 source_data
 */
static void invoke_channel_logic(int chnId, std::vector<AlgoResult> &current_results, int64_t frame_id,
                                 uint64_t timestamp_ms, uint64_t unix_ms, float dt_ms, int infer_enabled,
                                 const ChannelRawFrame *raw_frame,
                                 const std::shared_ptr<const AppRuntimeSnapshot> &runtime)
{
    const ChannelConfig *channel_config = app_ctrl_runtime_channel_config(runtime, chnId);
    const std::vector<RoiZone> *runtime_rois = app_ctrl_runtime_channel_rois(runtime, chnId);
    const LogicParameterSet *runtime_logic_parameters = app_ctrl_runtime_logic_parameters(runtime, chnId);
    if (!channel_config || !runtime_rois || !runtime_logic_parameters)
        return;
    const std::string &logic_name = channel_config->logic;

    /* 未配置后处理模块：不构造 ChannelContext，也不调用任何业务函数。
     * 仍提交严格同帧的 frame/results，供通用检测框绘制、快照和跨通道读取使用。 */
    if (logic_name.empty())
    {
        if (raw_frame && raw_frame->lazy_frame)
            raw_frame->lazy_frame->clear_borrowed_source();
        ChannelState &ch_state = g_pCtrl->channels_state[chnId];
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        ch_state.last_lazy_frame = raw_frame ? raw_frame->lazy_frame : nullptr;
        ch_state.logic_state.reset();
        ch_state.logic_outputs = empty_logic_output_snapshot();
        ch_state.last_results = current_results;
        ch_state.published_frame_seq = frame_id;
        ch_state.draw_cmds.clear();
        ch_state.logic_display_frame.release();
        ch_state.commit_publication(runtime, steady_now_ms(), timestamp_ms, unix_ms, infer_enabled,
                                    raw_frame ? raw_frame->width : 0, raw_frame ? raw_frame->height : 0);
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
        return;
    }

    ChannelLogicFunc fn = channel_logic_get(logic_name.c_str());
    if (!fn)
    {
        if (raw_frame && raw_frame->lazy_frame)
            raw_frame->lazy_frame->clear_borrowed_source();
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        ChannelState &state = g_pCtrl->channels_state[chnId];
        state.last_lazy_frame = raw_frame ? raw_frame->lazy_frame : nullptr;
        state.logic_state.reset();
        state.logic_outputs = empty_logic_output_snapshot();
        state.last_results = current_results;
        state.published_frame_seq = frame_id;
        state.draw_cmds.clear();
        state.logic_display_frame.release();
        state.commit_publication(runtime, steady_now_ms(), timestamp_ms, unix_ms, infer_enabled,
                                 raw_frame ? raw_frame->width : 0, raw_frame ? raw_frame->height : 0);
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
        return;
    }

    ChannelState &ch_state = g_pCtrl->channels_state[chnId];
    std::shared_ptr<void> logic_state;

    /* 构造 ChannelContext（栈上，logic 函数只在本次调用内使用）*/
    ChannelContext ctx;
    ctx.chnId = chnId;
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    ctx.src_width = raw_frame && raw_frame->width > 0 ? raw_frame->width : ch_state.src_w_now;
    ctx.src_height = raw_frame && raw_frame->height > 0 ? raw_frame->height : ch_state.src_h_now;
    ctx.disp_fps = ch_state.disp_fps;
    logic_state = ch_state.logic_state;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    ctx.frame_id = frame_id;
    ctx.timestamp_ms = timestamp_ms;
    /* RTSP/USB/文件统一透传业务帧进入分析管线时的墙钟，异步推理不会把完成时间误当帧时间。 */
    ctx.unix_ms = unix_ms;
    ctx.dt_ms = dt_ms;
    ctx.results = &current_results;
    ctx.config = channel_config;
    ctx.logic_parameters = runtime_logic_parameters;
    LogicOutputSet logic_outputs;
    ctx.outputs = &logic_outputs;
    /* 多 ROI 顶点均为模型坐标系。 */
    ctx.rois = runtime_rois;
    ctx.state = &logic_state;
    ctx.infer_enabled = infer_enabled;
    ctx.infer_fps = inference_get_infer_fps(chnId);

    /* model/source 两种尺寸共享同一个帧提供者，各自首次调用时才转换，且每帧最多一次。 */
    if (raw_frame && raw_frame->lazy_frame)
    {
        ctx.model_frame_getter = get_model_frame_bgr;
        ctx.source_frame_getter = get_source_frame_bgr;
        ctx.frame_getter_opaque = raw_frame->lazy_frame.get();
    }

    /* logic 在 chn_mtx 外运行。runtime shared_ptr 保证 config/ROI 在本帧全程有效；
     * ctx.state 由外层 g_process_mtx 保护，热更新只能在本帧 logic 返回后切换。 */
    std::vector<DrawCommand> draw_cmds;
    ctx.draw_cmds = &draw_cmds;
    /* 显示输出(可选): display_canvas()/replace_display_frame() 共用，不调用则零开销。 */
    cv::Mat canvas_buf;
    bool show_canvas = false;
    ctx.canvas = &canvas_buf;
    ctx.show_canvas = &show_canvas;
    std::vector<LogicAction> pending_actions;
    logic_control_take_channel(chnId, pending_actions);
    if (!pending_actions.empty())
    {
        ChannelLogicActionFunc action_fn = channel_logic_action_get(logic_name.c_str());
        for (const auto &action : pending_actions)
        {
            if (action.logic_name != logic_name)
            {
                printf("[ChannelAction][ch%02d][%s] drop action=%s request=%s (queued for %s)\n", chnId,
                       logic_name.c_str(), action.name.c_str(), action.request_id.c_str(), action.logic_name.c_str());
                continue;
            }
            if (!action_fn)
            {
                printf("[ChannelAction][ch%02d][%s] no handler for action=%s request=%s\n", chnId, logic_name.c_str(),
                       action.name.c_str(), action.request_id.c_str());
                continue;
            }
            LogicActionResult result = action_fn(&ctx, &action);
            printf("[ChannelAction][ch%02d][%s] action=%s request=%s handled=%d msg=%s\n", chnId, logic_name.c_str(),
                   action.name.c_str(), action.request_id.c_str(), result.handled ? 1 : 0, result.message.c_str());
        }
    }
    fn(&ctx);

    /* 同步回调借用的解码裸指针到这里即将失效。已经生成的 Mat 缓存及 DMA-BUF 句柄仍可安全持有。 */
    if (raw_frame && raw_frame->lazy_frame)
        raw_frame->lazy_frame->clear_borrowed_source();

    /* 堆分配在通道锁外完成，发布时只交换 shared_ptr。 */
    std::shared_ptr<const LogicOutputSet> published_outputs;
    if (logic_outputs.empty())
        published_outputs = empty_logic_output_snapshot();
    else
        published_outputs = std::make_shared<const LogicOutputSet>(std::move(logic_outputs));

    /* 原子写回共享状态：媒体快照在同一把锁内读出，三者必定同帧。*/
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        ch_state.last_lazy_frame = raw_frame ? raw_frame->lazy_frame : nullptr;
        ch_state.logic_state = std::move(logic_state);
        ch_state.logic_outputs = std::move(published_outputs);
        ch_state.last_results = current_results;
        ch_state.published_frame_seq = frame_id;
        ch_state.draw_cmds = std::move(draw_cmds);
        /* Logic 提交了显示帧 → 存为通道显示底图；否则清掉，显示回到实时采集帧。 */
        if (show_canvas && !canvas_buf.empty())
        {
            ch_state.logic_display_frame = std::move(canvas_buf);
            ch_state.logic_display_ts_ms = steady_now_ms();
        }
        else
        {
            ch_state.logic_display_frame.release();
        }
        ch_state.commit_publication(runtime, steady_now_ms(), timestamp_ms, unix_ms, infer_enabled,
                                    raw_frame ? raw_frame->width : 0, raw_frame ? raw_frame->height : 0);
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
 * （traditional logic worker / dispatch_worker 推理完成通知可能在开关切换期间同时触发）。
 */
std::vector<AlgoResult> process_channel_results(int chnId, const ChannelRawFrame &raw_frame,
                                                std::vector<AlgoResult> *new_results, int64_t result_frame_id)
{
    if (!g_pCtrl)
        return {};
    ChannelState &ch_state = g_pCtrl->channels_state[chnId];
    const auto runtime = app_ctrl_get_runtime_snapshot();
    const ChannelConfig *channel_config = app_ctrl_runtime_channel_config(runtime, chnId);
    if (!channel_config)
        return {};

    const uint64_t now_ms = steady_now_ms();
    const uint64_t logic_time_ms = raw_frame.frame_steady_ms != 0 ? raw_frame.frame_steady_ms : now_ms;

    int infer_enabled = config_utils::is_channel_infer_enabled(*channel_config) ? 1 : 0;
    if (infer_enabled)
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        infer_enabled = ch_state.infer_runtime_enable ? 1 : 0;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    }

    if (new_results)
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        const bool stale_after_reconnect =
            ch_state.online_ts_ms != 0 && raw_frame.frame_steady_ms != 0 &&
            raw_frame.frame_steady_ms <= ch_state.online_ts_ms;
        const bool reject_result = ch_state.online_state != CH_ONLINE || stale_after_reconnect;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
        if (reject_result)
            return {};
    }

    /* 运行时关闭推理后，丢弃关闭前仍在途的旧 NPU 结果；后续解码帧会走同步传统 CV 路径。 */
    if (!infer_enabled && new_results)
        return {};

    /* ---- 路径1：非推理通道 / 无结果直通 ---- */
    if (!new_results)
    {
        int64_t logic_frame_id = 0;
        float dt_ms = 0.0f;
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        logic_frame_id = ++ch_state.logic_frame_id;
        if (logic_frame_id > 1)
            dt_ms = logic_time_ms >= ch_state.last_logic_ts_ms
                        ? static_cast<float>(logic_time_ms - ch_state.last_logic_ts_ms)
                        : 0.0f;
        ch_state.last_logic_ts_ms = logic_time_ms;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);

        std::vector<AlgoResult> empty_results;
        const uint64_t frame_unix_ms = raw_frame.frame_unix_ms != 0 ? raw_frame.frame_unix_ms : system_now_ms();
        invoke_channel_logic(chnId, empty_results, logic_frame_id, logic_time_ms, frame_unix_ms, dt_ms,
                             infer_enabled, &raw_frame, runtime);
        return empty_results;
    }

    /* ---- 路径2：推理通道 ---- */
    std::vector<AlgoResult> results = *new_results;

    float dt_ms = 0.0f;
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
    const int64_t logic_frame_id = ++ch_state.logic_frame_id;
    if (logic_frame_id > 1)
        dt_ms = logic_time_ms >= ch_state.last_logic_ts_ms
                    ? static_cast<float>(logic_time_ms - ch_state.last_logic_ts_ms)
                    : 0.0f;
    ch_state.last_logic_ts_ms = logic_time_ms;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);

    if (Tracker *tracker = get_tracker(chnId, channel_config))
        tracker->update(results);

    const int64_t frame_seq = result_frame_id;
    const uint64_t frame_ts = raw_frame.frame_steady_ms != 0
                                  ? raw_frame.frame_steady_ms
                                  : (!results.empty() ? results.front().timestamp_ms : now_ms);
    const uint64_t frame_unix_ms = raw_frame.frame_unix_ms != 0 ? raw_frame.frame_unix_ms : system_now_ms();

    std::vector<AlgoResult> out = std::move(results);
    for (auto &result : out)
        result.chn_id = chnId;
    invoke_channel_logic(chnId, out, frame_seq, frame_ts, frame_unix_ms, dt_ms, infer_enabled, &raw_frame, runtime);
    return out;
}
