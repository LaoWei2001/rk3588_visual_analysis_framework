/**
 * @file result_dispatch.cpp
 * @brief NPU 推理结果分发线程
 *
 * dispatch_worker_thread 由 main 通过 pthread_create 启动，每推理通道一个。
 *
 * 数据流：
 *   infer_worker (NPU) → channel_results[chnId] + cv signal
 *   → algorithm_wait_result (阻塞等待) → algorithm_take_results (原子取帧+框)
 *   → process_channel_results → invoke_channel_logic → alarm_uploader_enqueue
 *
 * 帧匹配保证（此文件不涉及）：
 *   algorithm_take_results 在同一把锁下原子取出 640 输入图 (infer_frame)
 * 与检测框， 二者必定来自同一 frame_seq。process_channel_results
 * 将此匹配的对传给 logic。
 */

#include <cstdio>
#include <opencv2/opencv.hpp>
#include <pthread.h>
#include <vector>

#include "algoProcess.h"
#include "analyzer_internal.h"
#include "system.h"

/* 帧匹配诊断日志节流（每通道约 2 秒一次，由 debug_display 开关控制）*/
static uint64_t g_sync_dbg_last_ms[MAX_CHANNEL_NUM] = {0};
static constexpr uint64_t SYNC_DBG_WINDOW_MS = 2000;

extern "C" void *dispatch_worker_thread(void *arg)
{
    const int chnId = (int)(intptr_t)arg;

    while (g_dispatch_running)
    {
        /* 阻塞等待 NPU 完成通知（100ms 超时，防止退出时卡住）*/
        const int ready = algorithm_wait_result(chnId, 100);
        if (!g_dispatch_running)
            break;
        if (!ready)
            continue;

        /* 原子取出检测框与产生它的 640 输入图（同一把锁、同一 seq）*/
        std::vector<AlgoResult> current_results;
        cv::Mat infer_frame;
        int64_t result_frame_id = 0;
        const int has_new = algorithm_take_results(chnId, current_results, infer_frame, result_frame_id);
        if (!has_new)
            continue;

        /* 读最新解码帧信息（供 process_channel_results 兜底 + 诊断日志）*/
        ChannelRawFrame raw;
        int64_t input_seq_now = 0;
        {
            pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
            raw.width = g_pCtrl->channels_state[chnId].src_w_now;
            raw.height = g_pCtrl->channels_state[chnId].src_h_now;
            raw.model_input_mat = g_pCtrl->channels_state[chnId].last_frame;
            input_seq_now = g_pCtrl->channels_state[chnId].input_frame_seq;
            pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
        }

        /* 串行化同通道的 process_channel_results 调用
         * （与 videoOutHandle 非推理直通路径互斥，见 g_process_mtx 注释）*/
        pthread_mutex_lock(&g_process_mtx[chnId]);
        process_channel_results(chnId, raw, &current_results, &infer_frame, result_frame_id);
        pthread_mutex_unlock(&g_process_mtx[chnId]);

        /* 帧匹配诊断：result_seq vs 最新解码 input_seq，lag = 推理在途帧数 */
        const uint64_t dbg_now = steady_now_ms();
        if (dbg_now - g_sync_dbg_last_ms[chnId] >= SYNC_DBG_WINDOW_MS)
        {
            g_sync_dbg_last_ms[chnId] = dbg_now;
            DBG_PRINT("[FrameSync][ch%02d] result_seq=%lld input_seq=%lld lag=%lld "
                      "results=%zu\n",
                      chnId, (long long)result_frame_id, (long long)input_seq_now,
                      (long long)(input_seq_now - result_frame_id), current_results.size());
        }
    }

    return nullptr;
}
