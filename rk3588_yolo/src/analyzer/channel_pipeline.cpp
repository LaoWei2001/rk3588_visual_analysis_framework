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
#include "../logic/channel_logic.h"

/*======================== 跟踪器 (每通道一个实例) ========================*/

static std::unique_ptr<Tracker> g_trackers[MAX_CHANNEL_NUM];

static Tracker *get_tracker(int chnId)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM) return nullptr;
    const ChannelConfig &ch_cfg = g_pCtrl->config.channels[chnId];
    if (!ch_cfg.tracker_enable)
    {
        g_trackers[chnId].reset();
        return nullptr;
    }
    if (!g_trackers[chnId])
        g_trackers[chnId] = std::make_unique<Tracker>(
            ch_cfg.tracker_iou_thresh, ch_cfg.tracker_max_miss, ch_cfg.tracker_min_hits);
    return g_trackers[chnId].get();
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
    if (ch->tracker_enable)
    {
        if (g_trackers[chnId])
        {
            g_trackers[chnId]->setTrackerIoUThresh(ch->tracker_iou_thresh);
            g_trackers[chnId]->setTrackerMaxMiss(ch->tracker_max_miss);
        }
        else
        {
            g_trackers[chnId] = std::make_unique<Tracker>(
                ch->tracker_iou_thresh, ch->tracker_max_miss, ch->tracker_min_hits);
            printf("[ChannelPipeline] tracker enabled for ch%d (iou=%.2f, miss=%d)\n",
                   chnId, ch->tracker_iou_thresh, ch->tracker_max_miss);
        }
    }
    else
    {
        if (g_trackers[chnId])
        {
            g_trackers[chnId].reset();
            printf("[ChannelPipeline] tracker disabled for ch%d\n", chnId);
        }
    }
}

void analyzer_reset_tracker_ids(int chnId)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM) return;
    if (g_trackers[chnId])
    {
        g_trackers[chnId]->reset();
        printf("[ChannelPipeline] tracker state reset for ch%d\n", chnId);
    }
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
 */
static void invoke_channel_logic(int chnId,
                                  const cv::Mat &frame_for_logic,
                                  std::vector<AlgoResult> &current_results,
                                  int64_t frame_id,
                                  uint64_t timestamp_ms,
                                  float dt_ms,
                                  int infer_enabled)
{
    /* 取当前 logic 名称（热重载后可能已更换）*/
    std::string logic_name;
    {
        pthread_rwlock_rdlock(&g_pCtrl->mtx);
        logic_name = g_pCtrl->channels_state[chnId].logic_name;
        pthread_rwlock_unlock(&g_pCtrl->mtx);
    }

    ChannelLogicFunc fn = channel_logic_get(logic_name.c_str());
    if (!fn) return;

    ChannelState &ch_state = g_pCtrl->channels_state[chnId];

    /* 构造 ChannelContext（栈上，logic 函数只在本次调用内使用）*/
    ChannelContext ctx;
    ctx.chnId         = chnId;
    ctx.frame         = &frame_for_logic;
    ctx.src_width     = ch_state.src_w_now;   /* 原始视频分辨率(解码源帧尺寸, 如 1920×1080) */
    ctx.src_height    = ch_state.src_h_now;
    ctx.frame_id      = frame_id;
    ctx.timestamp_ms  = timestamp_ms;
    /* 墙上时钟(epoch ms): RTSP/USB/文件 三源统一在此盖一次, logic 读 ctx->unix_ms / time_hms()
     * 即得本帧真实时间。这是"处理本帧的时刻", 与采集相差一个管线延迟(对 HH:MM:SS 显示无感)。 */
    ctx.unix_ms       = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    ctx.dt_ms         = dt_ms;
    ctx.results       = &current_results;
    ctx.config        = &g_pCtrl->config.channels[chnId];
    /* 多 ROI: ctx.rois = 本通道全部区域; ctx.roi = 第一个区域(兼容老逻辑, 无区域时为 nullptr)。
     * 顶点已是模型坐标系(归一化加载时即是; 旧像素格式由下方 process_channel_results 缩放后填入)。*/
    ctx.rois          = &ch_state.roi_zones;
    ctx.roi           = ch_state.roi_zones.empty() ? nullptr : &ch_state.roi_zones[0].polygon;
    ctx.state         = &ch_state.logic_state;
    ctx.infer_enabled = infer_enabled;
    ctx.infer_fps     = algorithm_get_infer_fps(chnId);
    ctx.disp_fps      = ch_state.disp_fps;

    /* logic 在锁外运行：frame_for_logic/current_results/draw_cmds 均为调用栈本地变量；
     * ctx.roi 和 ctx.state 受外层 g_process_mtx 保护，ctx.config 与热重载共存
     * （config 指针在 logic 执行期间不会失效，只存在极低概率的值新旧混读，与改前相同）。
     * chn_mtx 只用于最终写回，持锁时间从 ms 级降至 μs 级。*/
    std::vector<DrawCommand> draw_cmds;
    ctx.draw_cmds = &draw_cmds;
    /* 显示画布(可选): logic 调 ctx->display_canvas() 才会启用并克隆，不调则零开销 */
    cv::Mat canvas_buf;
    bool    show_canvas = false;
    ctx.canvas      = &canvas_buf;
    ctx.show_canvas = &show_canvas;
    fn(&ctx);

    /* 原子写回共享状态：get_channel_snapshot() 在同一把锁内读出，三者必定同帧。*/
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        ch_state.last_logic_frame  = frame_for_logic;
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
 *   new_results == nullptr  → 非推理通道，用空结果直接走 logic（纯逻辑通道）
 *   new_results != nullptr  → 推理通道，先过 tracker，再走 logic
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
    {
        pthread_rwlock_rdlock(&g_pCtrl->mtx);
        infer_enabled = config_utils::is_channel_infer_enabled(
                            g_pCtrl->config.channels[chnId]) ? 1 : 0;
        pthread_rwlock_unlock(&g_pCtrl->mtx);
    }

    /* ROI 区域 → 逻辑坐标系(模型 640)。逻辑与显示都只用 roi_zones[i].polygon(模型坐标)，与视频源无关。
     * 归一化坐标已由 load_roi_zones_from_config 在初始化时 × 模型尺寸转换为模型坐标系。*/
    if (!ch_state.roi_model_space && !ch_state.roi_zones_raw.empty())
    {
        const int src_w   = raw_frame.width;
        const int src_h   = raw_frame.height;
        const int model_w = g_pCtrl->inputW;
        const int model_h = g_pCtrl->inputH;

        bool need = (src_w != ch_state.last_src_w || src_h != ch_state.last_src_h);
        if (!need)
            for (const auto &z : ch_state.roi_zones)
                if (z.polygon.empty()) { need = true; break; }   /* 首帧: 模型坐标尚未填充 */

        if (need)
        {
            const bool ok  = (src_w > 0 && src_h > 0 && model_w > 0 && model_h > 0);
            const float sx = ok ? (float)model_w / (float)src_w : 1.0f;
            const float sy = ok ? (float)model_h / (float)src_h : 1.0f;
            const size_t n = std::min(ch_state.roi_zones.size(), ch_state.roi_zones_raw.size());
            for (size_t i = 0; i < n; ++i)
            {
                auto       &dst = ch_state.roi_zones[i].polygon;
                const auto &raw = ch_state.roi_zones_raw[i];
                dst.clear();
                dst.reserve(raw.size());
                for (const auto &p : raw)
                    dst.emplace_back((int)(p.x * sx), (int)(p.y * sy));
            }
            ch_state.last_src_w = src_w;
            ch_state.last_src_h = src_h;
        }
    }

    /* ---- 路径1：非推理通道 / 无结果直通 ---- */
    if (!infer_enabled || !new_results)
    {
        ch_state.logic_frame_id++;
        const float dt_ms = (ch_state.logic_frame_id <= 1)
                            ? 0.0f
                            : (float)(now_ms - ch_state.last_logic_ts_ms);
        ch_state.last_logic_ts_ms = now_ms;

        std::vector<AlgoResult> empty_results;
        /* 防御：RGA 转换失败且无历史帧时 model_input_mat 为空。
         * 跳过 logic 调用，避免 logic 对空 cv::Mat 做矩阵运算崩溃。 */
        if (!raw_frame.model_input_mat.empty())
        {
            invoke_channel_logic(chnId, raw_frame.model_input_mat,
                                 empty_results, ch_state.logic_frame_id,
                                 now_ms, dt_ms, infer_enabled);
        }
        return empty_results;
    }

    /* ---- 路径2：推理通道 ---- */
    std::vector<AlgoResult> results = *new_results;

    ch_state.logic_frame_id++;
    const float dt_ms = (ch_state.logic_frame_id <= 1)
                        ? 0.0f
                        : (float)(now_ms - ch_state.last_logic_ts_ms);
    ch_state.last_logic_ts_ms = now_ms;

    if (Tracker *tracker = get_tracker(chnId))
        tracker->update(results);

    const int64_t  frame_seq = result_frame_id;
    const uint64_t frame_ts  = !results.empty() ? results.front().timestamp_ms : now_ms;

    const cv::Mat &frame_for_logic = (infer_frame && !infer_frame->empty())
                                         ? *infer_frame
                                         : raw_frame.model_input_mat;

    std::vector<AlgoResult> out = std::move(results);
    /* 防御：若 infer_frame 与 last_frame 均为空（极少见），跳过 logic 调用。*/
    if (frame_for_logic.empty())
        return out;
    invoke_channel_logic(chnId, frame_for_logic, out, frame_seq,
                         frame_ts, dt_ms, infer_enabled);
    return out;
}
