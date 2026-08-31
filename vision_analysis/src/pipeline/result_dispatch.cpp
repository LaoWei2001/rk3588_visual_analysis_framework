/**
 * @file result_dispatch.cpp
 * @brief NPU 推理结果分发线程
 *
 * pipeline_dispatch_worker 由 main 通过 pthread_create 启动，每推理通道一个。
 *
 * 数据流：
 *   infer_worker (NPU) → channel_results[chnId] + cv signal
 *   → inference_wait_result (阻塞等待) → inference_take_results (原子取惰性帧引用+框)
 *   → process_channel_results → invoke_channel_logic → event_report
 *
 * 帧匹配保证（此文件不涉及）：
 *   inference_take_results 在同一把锁下原子取出 LazyVideoFrame 与检测框，
 *   二者必定来自同一 frame_seq。Logic 真正调用取帧函数时才生成 BGR。
 */

#include <cstdio>
#include <memory>
#include <pthread.h>
#include <utility>
#include <vector>

#include "inference/inference_engine.h"
#include "pipeline_internal.h"
#include "frame_transform.h"
#include "common/logging.h"

/* 帧匹配诊断日志节流（每通道约 2 秒一次，由 debug_display 开关控制）*/
static uint64_t g_sync_dbg_last_ms[MAX_CHANNEL_NUM] = {0};
static constexpr uint64_t SYNC_DBG_WINDOW_MS = 2000;

extern "C" void *pipeline_dispatch_worker(void *arg)
{
    const int chnId = (int)(intptr_t)arg;

    while (g_dispatch_running.load())
    {
        /* 阻塞等待 NPU 完成通知（100ms 超时，防止退出时卡住）*/
        const int ready = inference_wait_result(chnId, 100);
        if (!g_dispatch_running.load())
            break;
        if (!ready)
            continue;

        /* 原子取出检测框与产生它的惰性帧引用（同一把锁、同一 seq）*/
        std::vector<AlgoResult> current_results;
        std::shared_ptr<LazyVideoFrame> lazy_frame;
        int64_t result_frame_id = 0;
        uint64_t result_frame_steady_ms = 0;
        uint64_t result_frame_unix_ms = 0;
        const int has_new = inference_take_results(chnId, current_results, lazy_frame, result_frame_id,
                                                   result_frame_steady_ms, result_frame_unix_ms);
        if (!has_new)
            continue;

        /* 组装与推理结果严格同帧的帧引用及元信息。 */
        ChannelRawFrame raw;
        raw.lazy_frame = std::move(lazy_frame);
        if (raw.lazy_frame)
        {
            raw.width = raw.lazy_frame->source_width();
            raw.height = raw.lazy_frame->source_height();
        }
        int64_t input_seq_now = 0;
        {
            pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
            if (raw.width <= 0)
                raw.width = g_pCtrl->channels_state[chnId].src_w_now;
            if (raw.height <= 0)
                raw.height = g_pCtrl->channels_state[chnId].src_h_now;
            raw.frame_steady_ms = result_frame_steady_ms;
            raw.frame_unix_ms = result_frame_unix_ms;
            input_seq_now = g_pCtrl->channels_state[chnId].input_frame_seq;
            pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
        }

        /* 串行化同通道的 process_channel_results 调用
         * （与 traditional logic worker 在推理开关切换期间互斥）*/
        pthread_mutex_lock(&g_process_mtx[chnId]);
        process_channel_results(chnId, raw, &current_results, result_frame_id);
        pthread_mutex_unlock(&g_process_mtx[chnId]);

        /* 帧匹配诊断：result_seq vs 最新解码 input_seq，lag = 推理在途帧数 */
        const uint64_t dbg_now = steady_now_ms();
        if (dbg_now - g_sync_dbg_last_ms[chnId] >= SYNC_DBG_WINDOW_MS)
        {
            g_sync_dbg_last_ms[chnId] = dbg_now;
            DBG_PRINT("[FrameSync][ch%02d] result_seq=%lld input_seq=%lld lag=%lld results=%zu\n", chnId,
                      (long long)result_frame_id, (long long)input_seq_now,
                      (long long)(input_seq_now - result_frame_id), current_results.size());
        }
    }

    return nullptr;
}
