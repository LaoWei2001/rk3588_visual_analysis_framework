/**
 * @file display_pipeline.cpp
 * @brief 异步显示线程
 *
 * display_worker_thread 由 main 通过 pthread_create 启动，每通道一个。
 *
 * 数据流：
 *   videoOutHandle (appsink 回调)
 *     → DispFramePool::back_buf (锁外 memcpy)
 *     → DispQueue::pool.publish (持锁整数槽交换)
 *     → pthread_cond_signal
 *   display_worker_thread
 *     → pthread_cond_wait
 *     → DispQueue::pool.swap_front_if_dirty (持锁整数槽交换)
 *     → DispFramePool::front_buf (锁外读取)
 *     → commitImgtoDispBufMap (RGA 缩放 + overlay + framebuffer)
 *
 * 与推理结果的关系（有意设计）：
 *   显示的是最新解码帧，叠加结果来自共享的 last_results（可能旧几帧），
 *   由 commitImgtoDispBufMap 内部用源帧时间与卡尔曼速度补偿框、姿态和逐目标掩码延迟。
 *   这是实时预览的合理取舍；logic/上报路径用严格同帧匹配的数据。
 *
 * 帧池设计要点（见 DispFramePool 注释）：
 *   持 DispQueue::mtx 的临界区只做整数级槽交换（≈10 ns），
 *   3 MB 的 memcpy 已由生产者在锁外提前完成。
 *   front 槽由本线程独占，commitImgtoDispBufMap 期间无需持锁。
 */

#include "analyzer_internal.h"
#include "frame_pipeline.h"
#include <pthread.h>

extern "C" void *display_worker_thread(void *arg)
{
    const int chnId = (int)(intptr_t)arg;
    DispQueue &dq = g_disp_queues[chnId];

    while (g_pCtrl && g_pCtrl->isRunning)
    {
        /* ---- 等待新帧（条件变量阻塞，零 CPU 占用）---- */
        DispTask task;
        bool reset_fps = false;
        {
            pthread_mutex_lock(&dq.mtx);
            while (!dq.has_task && g_pCtrl && g_pCtrl->isRunning)
                pthread_cond_wait(&dq.cv, &dq.mtx);
            if (!g_pCtrl || !g_pCtrl->isRunning)
            {
                pthread_mutex_unlock(&dq.mtx);
                break;
            }
            task = dq.task;                /* 仅拷贝元数据（6 个整数，约 24 B）*/
            reset_fps = dq.reset_fps_pending;
            dq.reset_fps_pending = false;
            dq.pool.swap_front_if_dirty(); /* mid↔front 整数交换，将最新帧切为 front */
            dq.has_task = 0;
            pthread_mutex_unlock(&dq.mtx);
        }

        if (reset_fps)
        {
            /* fps_counter/last_fps_ts_ms 由本线程独占；共享显示值在通道锁下清零。
             * 第一帧随后立即进入新的统计窗口，不包含程序启动或 RTSP 断连时间。 */
            ChannelState &state = g_pCtrl->channels_state[chnId];
            state.fps_counter = 0;
            state.last_fps_ts_ms = steady_now_ms();
            pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
            state.disp_fps = 0.0f;
            pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
        }

        /* ---- RGA 缩放 + render_overlays + 写 framebuffer ----
         * front_buf() 无需持锁：
         *   生产者只写 back 槽（back_idx ≠ front_idx 始终成立），
         *   front 槽由本线程独占直到下次 swap_front_if_dirty。
         *
         * overlay 在 commitImgtoDispBufMap 内读取共享 last_results，
         * 按结果源帧到当前显示帧的真实时间差做运动外推。*/
        commitImgtoDispBufMap(task.chnId, dq.pool.front_buf(), task.srcFmt, task.srcWidth, task.srcHeight,
                              task.srcHStride, task.srcVStride, task.frameSteadyMs);
    }

    return nullptr;
}
