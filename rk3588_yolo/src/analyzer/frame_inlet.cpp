/**
 * @file frame_inlet.cpp
 * @brief 视频帧入口
 *
 * videoOutHandle 是 GStreamer appsink 回调调用的唯一帧入口，每解码一帧就调用一次。
 *
 * 每帧做三件事（顺序固定）：
 *   1. FPS 节流 + 交错调度（phase-offset）：按 max_fps 决定本帧是否"处理"（推理通道与传统算法通道都限频）
 *   2. RGA 转换：NV12 → BGR 640×640（convertToYoloInput；仅"处理"的帧才转）
 *   3. 分流：
 *      - 推理通道(infer_enable 且配置了模型)：algorithm_process_mat → TaskQueue → infer_worker
 *      - 传统算法通道(infer_enable=false)：同步调 process_channel_results（持 g_process_mtx，ctx->results 为空）
 *      - 显示：不论是否推理，均将最新解码帧推入 g_disp_queues（单槽覆盖）
 *
 * 每 5 秒打印一次每通道的 recv/throttle/enq/drop/conv 统计。
 */

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <vector>
#include <pthread.h>
#include <opencv2/opencv.hpp>

#include "system.h"
#include "analyzer_internal.h"
#include "analyzer.h"
#include "frame_pipeline.h"
#include "algoProcess.h"
#include "../core/pause_ctrl.h"

/*======================== 送帧统计（每通道，仅 videoOutHandle 访问）========================*/

struct FeedStats
{
    uint64_t recv      = 0;  /* appsink 收到的总帧数 */
    uint64_t enq       = 0;  /* 成功入推理队列的帧数 */
    uint64_t drop      = 0;  /* 推理队列满、被丢弃的帧数 */
    uint64_t conv_fail = 0;  /* RGA/CPU 转换失败的帧数 */
    uint64_t conv_ok   = 0;  /* 转换成功的帧数 */
    uint64_t conv_us   = 0;  /* 转换耗时累计（微秒） */
    uint64_t throttle  = 0;  /* FPS 节流跳过的帧数 */
    uint64_t log_last_ms = 0;
    uint64_t next_due_us = 0; /* FPS 节流：下次允许推理的时刻（微秒） */
};

static FeedStats        g_feed[MAX_CHANNEL_NUM];
static pthread_mutex_t  g_feed_mtx = PTHREAD_MUTEX_INITIALIZER;
static constexpr uint64_t FEED_LOG_WINDOW_MS = 5000;

/*======================== 节流计时重置（供 analyzer_channel_offline/online 调用）========================*/

void feed_stats_reset(int chnId)
{
    if (chnId < 0 || chnId >= MAX_CHANNEL_NUM) return;
    pthread_mutex_lock(&g_feed_mtx);
    g_feed[chnId].next_due_us = 0;  /* 下次帧到达时重新计算 phase-offset */
    pthread_mutex_unlock(&g_feed_mtx);
}

/*======================== videoOutHandle ========================*/

int videoOutHandle(char *imgData, ImgDesc_t imgDesc)
{
    if (pause_ctrl::is_paused()) return 0;
    if (imgDesc.chnId < 0 || imgDesc.chnId >= MAX_CHANNEL_NUM) return -1;

    const int ch      = imgDesc.chnId;
    const int fmt_int = rgaFmt(imgDesc.fmt);

    /* ---- 查推理开关 ---- */
    int infer_enabled = 0;
    {
        pthread_rwlock_rdlock(&g_pCtrl->mtx);
        if (ch < app_ctrl_get_chn_nums())
            infer_enabled = config_utils::is_channel_infer_enabled(
                                g_pCtrl->config.channels[ch]) ? 1 : 0;
        pthread_rwlock_unlock(&g_pCtrl->mtx);
    }

    /* ---- FPS 节流（phase-offset 交错调度）----
     * 对推理通道与传统算法通道(infer_enable=false)都生效：后者也要按 max_fps 限频跑 logic
     * (需要 640 BGR 帧)，而不是每解码帧都跑。是否真正进 NPU 另由 infer_enabled 单独控制。*/
    int will_process = 0;
    uint64_t throttle_period_us = 0;
    {
        int max_fps;
        {
            pthread_rwlock_rdlock(&g_pCtrl->mtx);
            max_fps = std::max(1, g_pCtrl->config.channels[ch].max_fps);
            pthread_rwlock_unlock(&g_pCtrl->mtx);
        }
        const uint64_t period_us = (uint64_t)(1000000 / max_fps);
        throttle_period_us       = period_us;
        const uint64_t now_us    = steady_now_us();
        uint64_t due_us          = g_feed[ch].next_due_us;

        while (true)
        {
            if (due_us == 0)
            {
                /* 首次：按通道号错开相位，避免所有通道在同一时刻触发 */
                const int chn_cnt = std::max(1, app_ctrl_get_chn_nums());
                const uint64_t phase_us = (period_us * (uint64_t)(ch % chn_cnt)) / (uint64_t)chn_cnt;
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
                break;          /* 未到时刻，本帧跳过推理 */
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

        if (will_process && infer_enabled)
            g_pCtrl->channels_state[ch].last_infer_ts_ms = steady_now_ms();
    }

    /* ---- RGA 转换：NV12 → BGR 640×640 ----
     * 即使 NPU 推理会走 fd 零拷贝路径，也必须生成 CPU 侧的 yolo_input (cv::Mat)，
     * 因为 logic/上报路径需要 ctx->frame（640 BGR Mat）作为图像底图。*/
    cv::Mat  yolo_input;
    int      conv_ok = 0;
    uint64_t conv_us = 0;
    if (will_process)
    {
        const auto conv_begin = std::chrono::steady_clock::now();
        conv_ok = convertToYoloInput(ch, imgData, imgDesc.fd,
                                     imgDesc.width, imgDesc.height,
                                     imgDesc.horStride, imgDesc.verStride,
                                     fmt_int, yolo_input);
        conv_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - conv_begin).count();
    }

    /* ---- 生成单调递增 frame_seq ---- */
    int64_t current_frame_seq = 0;
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[ch]);
        current_frame_seq = ++g_pCtrl->channels_state[ch].input_frame_seq;
        /* 记录真实解码源分辨率：推理通道的 ROI 缩放(result_dispatch → process_channel_results)
         * 靠它把旧像素格式 ROI 从源像素缩到模型 640 坐标系；否则 src_w 停在 0、roi_zones 不缩放，
         * 判定永远落在框外(画面只见显示用黄框、判定却 CLEAR)。(归一化 ROI 加载即是模型坐标, 不受此影响)*/
        g_pCtrl->channels_state[ch].src_w_now = imgDesc.width;
        g_pCtrl->channels_state[ch].src_h_now = imgDesc.height;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[ch]);
    }

    /* ---- 构造 ChannelRawFrame（供非推理直通路径 + dispatch 兜底）---- */
    ChannelRawFrame raw_frame;
    raw_frame.width  = imgDesc.width;
    raw_frame.height = imgDesc.height;
    if (conv_ok && !yolo_input.empty())
        raw_frame.model_input_mat = yolo_input;
    else
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[ch]);
        if (!g_pCtrl->channels_state[ch].last_frame.empty())
            raw_frame.model_input_mat = g_pCtrl->channels_state[ch].last_frame;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[ch]);
    }

    /* ---- 传统算法通道(infer_enable=false)：节流命中且有帧时同步跑 logic ----
     * 与推理通道一样按 max_fps 限频；raw_frame.model_input_mat 为 640 BGR(本帧转换或历史兜底)，
     * 使 ctx->frame 可用于传统视觉(HSV/帧差等)；不进 NPU、ctx->results 为空、ctx->infer_enabled=0。*/
    if (!infer_enabled && will_process)
    {
        pthread_mutex_lock(&g_process_mtx[ch]);
        process_channel_results(ch, raw_frame, nullptr, nullptr, current_frame_seq);
        pthread_mutex_unlock(&g_process_mtx[ch]);
    }

    /* ---- 推入显示队列（单槽覆盖，始终展示最新解码帧）----
     * 开启 RTSP 推流时即使没接显示器(enable_display=false)也要合成，
     * 因为 RTSP 取流自同一张 g_disp 拼接大图。 */
    if ((app_ctrl_get_enable_disp() || app_ctrl_get_enable_rtsp()) && g_pCtrl->pDispBuffer &&
        *g_pCtrl->pDispBuffer && imgData)
    {
        size_t data_size = 0;
        if (fmt_int == RK_FORMAT_YCbCr_420_SP || fmt_int == RK_FORMAT_YCrCb_420_SP)
            data_size = (size_t)imgDesc.horStride * imgDesc.verStride * 3 / 2;
        else if (fmt_int == RK_FORMAT_BGR_888 || fmt_int == RK_FORMAT_RGB_888)
            data_size = (size_t)imgDesc.horStride * imgDesc.verStride * 3;

        if (data_size > 0)
        {
            DispQueue &dq = g_disp_queues[ch];
            /* ① 锁外 memcpy：3 MB 拷贝不再阻塞显示线程取帧
             *   back 槽由本通道的 videoOutHandle（单一生产者）独占，
             *   无需额外加锁。*/
            uint8_t *dst = dq.pool.back_buf(data_size);
            if (dst)
            {
                memcpy(dst, imgData, data_size);
                /* ② 持锁仅做元数据更新 + 整数槽交换（≈20 ns） */
                pthread_mutex_lock(&dq.mtx);
                dq.task.chnId      = ch;
                dq.task.srcFmt     = fmt_int;
                dq.task.srcWidth   = imgDesc.width;
                dq.task.srcHeight  = imgDesc.height;
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

    /* ---- 推理通道：送入 TaskQueue，由 infer_worker 异步执行 ---- */
    if (will_process && infer_enabled)
    {
        if (conv_ok)
        {
            g_feed[ch].conv_ok++;
            g_feed[ch].conv_us += conv_us;
            const int enq_ret = algorithm_process_mat(
                ch, std::move(yolo_input),
                imgDesc.fd, imgDesc.width, imgDesc.height,
                fmt_int, imgDesc.horStride, imgDesc.verStride,
                current_frame_seq);
            if (enq_ret > 0)
                g_feed[ch].enq++;
            else
            {
                g_feed[ch].drop++;
                /* 队列满：小步快追，下次提前允许推理 */
                if (throttle_period_us > 0)
                    g_feed[ch].next_due_us = steady_now_us() + throttle_period_us / 2ULL;
            }
        }
        else
        {
            g_feed[ch].conv_fail++;
        }
    }

    /* ---- 周期性统计日志（每 5 秒一次）---- */
    const uint64_t now_ms   = steady_now_ms();
    const uint64_t last_ms  = g_feed[ch].log_last_ms;
    if (last_ms == 0)
    {
        g_feed[ch].log_last_ms = now_ms;
    }
    else if (now_ms - last_ms >= FEED_LOG_WINDOW_MS)
    {
        g_feed[ch].log_last_ms = now_ms;

        const uint64_t recv_s      = g_feed[ch].recv;      g_feed[ch].recv      = 0;
        const uint64_t enq_s       = g_feed[ch].enq;       g_feed[ch].enq       = 0;
        const uint64_t drop_s      = g_feed[ch].drop;      g_feed[ch].drop      = 0;
        const uint64_t conv_fail_s = g_feed[ch].conv_fail; g_feed[ch].conv_fail = 0;
        const uint64_t conv_ok_s   = g_feed[ch].conv_ok;   g_feed[ch].conv_ok   = 0;
        const uint64_t conv_us_s   = g_feed[ch].conv_us;   g_feed[ch].conv_us   = 0;
        const uint64_t throttle_s  = g_feed[ch].throttle;  g_feed[ch].throttle  = 0;

        const uint64_t q_total_s  = enq_s + drop_s;
        const float q_drop_rate   = q_total_s > 0
                                    ? (100.0f * (float)drop_s / (float)q_total_s) : 0.0f;
        const float conv_avg_ms   = conv_ok_s > 0
                                    ? ((float)conv_us_s / (float)conv_ok_s / 1000.0f) : 0.0f;
        const float infer_fps_val = algorithm_get_infer_fps(ch);

        int show_perf = 0;
        {
            pthread_rwlock_rdlock(&g_pCtrl->mtx);
            show_perf = g_pCtrl->config.performance_display;
            pthread_rwlock_unlock(&g_pCtrl->mtx);
        }
        if (show_perf)
        {
            log_printf_threadsafe(
                "[Feed][ch%02d][5s] recv=%llu throttle=%llu enq=%llu "
                "qdrop=%llu(%.1f%%) conv_ok=%llu conv_fail=%llu "
                "conv_avg=%.2fms infer=%.1ffps\n",
                ch,
                (unsigned long long)recv_s,
                (unsigned long long)throttle_s,
                (unsigned long long)enq_s,
                (unsigned long long)drop_s,   q_drop_rate,
                (unsigned long long)conv_ok_s,
                (unsigned long long)conv_fail_s,
                conv_avg_ms, infer_fps_val);
        }
    }

    return 0;
}
