/**
 * @file frame_inlet.cpp
 * @brief 视频帧入口
 *
 * pipeline_submit_frame 是 GStreamer appsink 回调调用的唯一帧入口，每解码一帧就调用一次。
 *
 * 每帧做三件事（顺序固定）：
 *   1. FPS 节流 + 交错调度（phase-offset）：按 max_fps 决定本帧是否"处理"（推理通道与传统算法通道都限频）
 *   2. 只传递稳定的 DMA-BUF 引用；Logic 调用 model_frame()/source_frame() 时才生成 BGR
 *   3. 分流：
 *      - 推理通道(infer_enable 且配置了模型)：inference_process_source → TaskQueue → infer_worker
 *      - 传统算法通道(infer_enable=false)：发布最新帧给独立 logic worker
 *      - 显示：不论是否推理，均将最新解码帧推入 g_display_queues（单槽覆盖）
 *
 * 每 5 秒打印一次每通道的 recv/throttle/enq/replace/drop 统计。
 */

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <memory>
#include <pthread.h>
#include <utility>

#include "runtime/pause_ctrl.h"
#include "recorder/event_video_recorder.h"
#include "rtsp/rtsp_streamer.h"
#include "inference/inference_engine.h"
#include "pipeline_runtime.h"
#include "pipeline_internal.h"
#include "frame_transform.h"
#include "common/logging.h"

/*======================== 送帧统计（每通道，仅 pipeline_submit_frame 访问）========================*/

struct FeedStats
{
    uint64_t recv = 0;      /* appsink 收到的总帧数 */
    uint64_t enq = 0;       /* 成功入推理队列的帧数 */
    uint64_t replace = 0;   /* 用新帧替换尚未处理的旧 pending 帧 */
    uint64_t drop = 0;      /* 未能送入推理引擎的帧数（停机、重载或帧导入失败） */
    uint64_t throttle = 0;  /* FPS 节流跳过的帧数 */
    uint64_t log_last_ms = 0;
    uint64_t next_due_us = 0; /* FPS 节流：下次允许推理的时刻（微秒） */
    uint64_t preview_token_ts_us = 0; /* 自动预览令牌桶上次补充时间 */
    double preview_tokens = 1.0;      /* 小容量令牌桶：吸收实时流到帧抖动，不积累画面 */
};

static FeedStats g_feed[MAX_CHANNEL_NUM];
static pthread_mutex_t g_feed_mtx = PTHREAD_MUTEX_INITIALIZER;
static constexpr uint64_t FEED_LOG_WINDOW_MS = 5000;

/*======================== 节流计时重置（供 pipeline_channel_offline/online 调用）========================*/

void feed_stats_reset(int chnId)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM)
        return;
    pthread_mutex_lock(&g_feed_mtx);
    g_feed[chnId].next_due_us = 0; /* 下次帧到达时重新计算 phase-offset */
    g_feed[chnId].preview_token_ts_us = 0;
    g_feed[chnId].preview_tokens = 1.0;
    pthread_mutex_unlock(&g_feed_mtx);
}

/*======================== pipeline_submit_frame ========================*/

int pipeline_submit_frame(char *imgData, FrameInputDesc imgDesc)
{
    if (pause_ctrl::is_paused())
        return 0;
    if (!app_ctrl_has_channel(imgDesc.chnId))
        return -1;

    const int ch = imgDesc.chnId;
    const int fmt_int = rga_format_from_name(imgDesc.fmt);
    const uint64_t frame_steady_ms = steady_now_ms();
    const uint64_t frame_unix_ms = system_now_ms();
    const auto runtime = app_ctrl_get_runtime_snapshot();
    const ChannelConfig *channel_config = app_ctrl_runtime_channel_config(runtime, ch);
    if (!channel_config)
        return -1;

    /* 原始分辨率事件录像入口。函数内部按配置FPS节流，回调线程只复制命中的源帧；
     * 颜色转换、JPEG环形缓冲和MP4编码均在录像线程执行。 */
    bool record_enabled = false;
    int record_fps = 5;
    float record_pre_sec = 5.0f;
    std::string record_overlay = "none";
    record_enabled = channel_config->event_video.enable;
    record_fps = channel_config->event_video.fps;
    record_pre_sec = channel_config->event_video.pre_sec;
    record_overlay = channel_config->event_video.overlay;
    /* 所有录像模式都从解码源帧进入独立录像队列。DISPLAY 模式由录像工作线程
     * 使用与实时播放相同的尺寸和渲染函数，不依赖 display/RTSP 是否启用。 */
    if (record_enabled)
    {
        EventVideoOverlayMode overlay_mode = EVENT_VIDEO_OVERLAY_NONE;
        if (record_overlay == "custom" || record_overlay == "all")
            overlay_mode = EVENT_VIDEO_OVERLAY_DISPLAY;
        event_video_recorder_push_source_frame(ch, imgData, fmt_int, imgDesc.width, imgDesc.height, imgDesc.horStride,
                                               imgDesc.verStride, record_fps, record_pre_sec, overlay_mode);
    }

    /* ---- 查推理开关 ---- */
    int infer_enabled = 0;
    infer_enabled = config_utils::is_channel_infer_enabled(*channel_config) ? 1 : 0;
    /* 运行时开关: 系统级动作 infer_toggle 可逐通道强制关闭推理(画面正常显示) */
    if (infer_enabled)
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[ch]);
        if (!g_pCtrl->channels_state[ch].infer_runtime_enable)
            infer_enabled = 0;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[ch]);
    }

    /* ---- FPS 节流（phase-offset 交错调度）----
     * 对推理通道与传统算法通道(infer_enable=false)都生效：后者也要按 max_fps 限频跑 logic
     * (需要 640 BGR 帧)，而不是每解码帧都跑。是否真正进 NPU 另由 infer_enabled 单独控制。*/
    int will_process = 0;
    uint64_t throttle_period_us = 0;
    {
        const int max_fps = std::max(1, channel_config->max_fps);
        const uint64_t period_us = (uint64_t)(1000000 / max_fps);
        throttle_period_us = period_us;
        const uint64_t now_us = steady_now_us();
        uint64_t due_us = g_feed[ch].next_due_us;

        while (true)
        {
            if (due_us == 0)
            {
                /* 首次：按通道号错开相位，避免所有通道在同一时刻触发 */
                const int chn_cnt = std::max(1, app_ctrl_get_chn_nums());
                const int display_order = std::max(0, app_ctrl_get_channel_display_order(ch));
                const uint64_t phase_us =
                    (period_us * static_cast<uint64_t>(display_order)) / static_cast<uint64_t>(chn_cnt);
                const uint64_t init_due = now_us + phase_us;
                pthread_mutex_lock(&g_feed_mtx);
                if (g_feed[ch].next_due_us == 0)
                    g_feed[ch].next_due_us = init_due;
                due_us = g_feed[ch].next_due_us;
                pthread_mutex_unlock(&g_feed_mtx);
                continue;
            }
            if (now_us < due_us)
            {
                g_feed[ch].throttle++;
                break; /* 未到时刻，本帧跳过推理 */
            }
            /* 正常推进；若落后超过一个周期则小步快追 */
            uint64_t next_due = due_us + period_us;
            if (now_us > due_us + period_us * 2ULL)
                next_due = now_us + period_us / 2ULL;
            pthread_mutex_lock(&g_feed_mtx);
            g_feed[ch].next_due_us = next_due;
            pthread_mutex_unlock(&g_feed_mtx);
            will_process = 1;
            break;
        }
    }

    /* ---- 生成单调递增 frame_seq ---- */
    int64_t current_frame_seq = 0;
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[ch]);
        current_frame_seq = ++g_pCtrl->channels_state[ch].input_frame_seq;
        /* 记录真实解码源分辨率，供 ChannelContext 元信息、结果分发兜底和源图/模型坐标换算使用。
         * 当前 roi_zones 配置统一为归一化坐标，并在运行快照发布时直接换算到模型坐标系。 */
        g_pCtrl->channels_state[ch].src_w_now = imgDesc.width;
        g_pCtrl->channels_state[ch].src_h_now = imgDesc.height;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[ch]);
    }

    /* ---- 构造 ChannelRawFrame（供非推理异步路径 + dispatch 兜底）---- */
    ChannelRawFrame raw_frame;
    raw_frame.width = imgDesc.width;
    raw_frame.height = imgDesc.height;
    raw_frame.frame_steady_ms = frame_steady_ms;
    raw_frame.frame_unix_ms = frame_unix_ms;
    if (!infer_enabled && will_process)
    {
        auto imported = rga_import_src_fd(imgDesc.fd, imgDesc.width, imgDesc.height, imgDesc.horStride,
                                          imgDesc.verStride, fmt_int);
        const bool has_imported_source = static_cast<bool>(imported);
        raw_frame.lazy_frame = std::make_shared<LazyVideoFrame>(
            ch, std::move(imported), imgDesc.width, imgDesc.height, imgDesc.horStride, imgDesc.verStride, fmt_int,
            g_pCtrl->inputW, g_pCtrl->inputH, has_imported_source ? nullptr : imgData);
        if (!has_imported_source)
        {
            /* 无 DMA-BUF 时只保留原始字节，不在回调里做 BGR/缩放。这使文件、USB
             * 和软件解码源也保持“appsink 只送帧”的底层约束。 */
            size_t source_bytes = imgDesc.dataSize > 0 ? static_cast<size_t>(imgDesc.dataSize) : 0U;
            if (source_bytes == 0 && (fmt_int == RK_FORMAT_YCbCr_420_SP || fmt_int == RK_FORMAT_YCrCb_420_SP))
                source_bytes = static_cast<size_t>(imgDesc.horStride) * imgDesc.verStride * 3U / 2U;
            else if (source_bytes == 0 && (fmt_int == RK_FORMAT_BGR_888 || fmt_int == RK_FORMAT_RGB_888))
                source_bytes = static_cast<size_t>(imgDesc.horStride) * imgDesc.verStride * 3U;
            if (!raw_frame.lazy_frame->retain_borrowed_source(source_bytes))
                raw_frame.lazy_frame.reset();
        }
    }

    /* ---- 分析任务优先入队 ----
     * NPU 与传统 CV 都在预览整帧拷贝之前发布，可与后续显示处理重叠执行。 */
    if (will_process && infer_enabled)
    {
        const int enq_ret = inference_process_source(
            ch, imgData, imgDesc.fd, imgDesc.width, imgDesc.height, fmt_int, imgDesc.horStride, imgDesc.verStride,
            current_frame_seq, raw_frame.frame_steady_ms, raw_frame.frame_unix_ms);
        if (enq_ret > 0)
        {
            g_feed[ch].enq++;
            if (enq_ret == 2)
                g_feed[ch].replace++;
        }
        else
        {
            g_feed[ch].drop++;
            if (throttle_period_us > 0)
                g_feed[ch].next_due_us = steady_now_us() + throttle_period_us / 2ULL;
        }
    }
    else if (will_process && raw_frame.lazy_frame)
        traditional_logic_publish(ch, std::move(raw_frame), current_frame_seq);

    /* ---- 推入显示队列（单槽覆盖，始终展示最新解码帧）----
     * HDMI 常开；纯 RTSP 模式只在存在实际客户端时合成，空闲期间不做整帧复制、缩放和叠加绘制。 */
    const bool preview_consumer_active =
        app_ctrl_get_enable_disp() || (app_ctrl_get_enable_rtsp() && rtsp_streamer_has_active_client());
    bool preview_due = false;
    if (preview_consumer_active)
    {
        const uint64_t now_us = steady_now_us();
        const bool preview_restarted = g_feed[ch].preview_token_ts_us == 0;
        if (preview_restarted)
        {
            g_feed[ch].preview_token_ts_us = now_us;
            g_feed[ch].preview_tokens = 1.0;

            /* 预览重新开始时由 display_worker 建立新的统计窗口，不把等待客户端
             * 的空闲时间计入 FPS。 */
            DisplayQueue &dq = g_display_queues[ch];
            pthread_mutex_lock(&dq.mtx);
            dq.reset_fps_pending = true;
            pthread_mutex_unlock(&dq.mtx);
        }
        else
        {
            const uint64_t elapsed_us = now_us >= g_feed[ch].preview_token_ts_us
                                            ? now_us - g_feed[ch].preview_token_ts_us
                                            : 0;
            g_feed[ch].preview_token_ts_us = now_us;
            const double refill = static_cast<double>(elapsed_us) * constants::PREVIEW_MAX_FPS / 1000000.0;
            g_feed[ch].preview_tokens = std::min(2.0, g_feed[ch].preview_tokens + refill);
        }

        if (g_feed[ch].preview_tokens >= 1.0)
        {
            g_feed[ch].preview_tokens -= 1.0;
            preview_due = true;
        }
    }
    else
    {
        /* 下次出现消费者时以一个初始令牌立即交付首帧，不继承空闲期间额度。 */
        g_feed[ch].preview_token_ts_us = 0;
        g_feed[ch].preview_tokens = 1.0;
    }

    if (preview_due && g_pCtrl->pDispBuffer && *g_pCtrl->pDispBuffer && imgData)
    {
        size_t data_size = 0;
        if (fmt_int == RK_FORMAT_YCbCr_420_SP || fmt_int == RK_FORMAT_YCrCb_420_SP)
            data_size = (size_t)imgDesc.horStride * imgDesc.verStride * 3 / 2;
        else if (fmt_int == RK_FORMAT_BGR_888 || fmt_int == RK_FORMAT_RGB_888)
            data_size = (size_t)imgDesc.horStride * imgDesc.verStride * 3;

        if (data_size > 0)
        {
            DisplayQueue &dq = g_display_queues[ch];
            /* ① 锁外 memcpy：3 MB 拷贝不再阻塞显示线程取帧
             *   back 槽由本通道的 pipeline_submit_frame（单一生产者）独占，
             *   无需额外加锁。*/
            uint8_t *dst = dq.pool.back_buf(data_size);
            if (dst)
            {
                memcpy(dst, imgData, data_size);
                /* ② 持锁仅做元数据更新 + 整数槽交换（≈20 ns） */
                pthread_mutex_lock(&dq.mtx);
                dq.task.chnId = ch;
                dq.task.srcFmt = fmt_int;
                dq.task.srcWidth = imgDesc.width;
                dq.task.srcHeight = imgDesc.height;
                dq.task.srcHStride = imgDesc.horStride;
                dq.task.srcVStride = imgDesc.verStride;
                dq.pool.publish();
                dq.has_task = 1;
                pthread_mutex_unlock(&dq.mtx);
                pthread_cond_signal(&dq.cv);
            }
        }
    }

    /* ---- 统计 recv ---- */
    g_feed[ch].recv++;

    /* ---- 周期性统计日志（每 5 秒一次）---- */
    const uint64_t now_ms = steady_now_ms();
    const uint64_t last_ms = g_feed[ch].log_last_ms;
    if (last_ms == 0)
    {
        g_feed[ch].log_last_ms = now_ms;
    }
    else if (now_ms - last_ms >= FEED_LOG_WINDOW_MS)
    {
        g_feed[ch].log_last_ms = now_ms;

        const uint64_t recv_s = g_feed[ch].recv;
        g_feed[ch].recv = 0;
        const uint64_t enq_s = g_feed[ch].enq;
        g_feed[ch].enq = 0;
        const uint64_t drop_s = g_feed[ch].drop;
        g_feed[ch].drop = 0;
        const uint64_t replace_s = g_feed[ch].replace;
        g_feed[ch].replace = 0;
        const uint64_t throttle_s = g_feed[ch].throttle;
        g_feed[ch].throttle = 0;

        const uint64_t q_total_s = enq_s + drop_s;
        const float q_drop_rate = q_total_s > 0 ? (100.0f * (float)drop_s / (float)q_total_s) : 0.0f;
        const float infer_fps_val = inference_get_infer_fps(ch);

        const int show_perf = app_ctrl_get_performance_display();
        if (show_perf)
        {
            log_printf_threadsafe("[Feed][ch%02d][5s] recv=%llu throttle=%llu enq=%llu replace=%llu "
                                  "qdrop=%llu(%.1f%%) infer=%.1ffps\n",
                                  ch, (unsigned long long)recv_s, (unsigned long long)throttle_s,
                                  (unsigned long long)enq_s, (unsigned long long)replace_s,
                                  (unsigned long long)drop_s, q_drop_rate, infer_fps_val);
        }
    }

    return 0;
}
