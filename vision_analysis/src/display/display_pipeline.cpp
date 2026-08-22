/**
 * @file display_pipeline.cpp
 * @brief 异步显示线程
 *
 * display_worker_thread 由 main 通过 pthread_create 启动，每通道一个。
 *
 * 数据流：
 *   pipeline_submit_frame (appsink 回调)
 *     → DisplayFramePool::back_buf (锁外 memcpy)
 *     → DisplayQueue::pool.publish (持锁整数槽交换)
 *     → pthread_cond_signal
 *   display_worker_thread
 *     → pthread_cond_wait
 *     → DisplayQueue::pool.swap_front_if_dirty (持锁整数槽交换)
 *     → DisplayFramePool::front_buf (锁外读取)
 *     → display_commit_frame (RGA 缩放 + overlay + framebuffer)
 *
 * 与推理结果的关系（有意设计）：
 *   显示的是最新解码帧，叠加的框来自共享的 last_results（可能旧几帧），
 *   由 display_commit_frame 内部用卡尔曼速度外推补偿管线延迟。
 *   这是实时预览的合理取舍；logic/上报路径用严格同帧匹配的数据。
 *
 * 帧池设计要点（见 DisplayFramePool 注释）：
 *   持 DisplayQueue::mtx 的临界区只做整数级槽交换（≈10 ns），
 *   3 MB 的 memcpy 已由生产者在锁外提前完成。
 *   front 槽由本线程独占，display_commit_frame 期间无需持锁。
 */

#include "display_pipeline.h"
#include "pipeline/pipeline_internal.h"
#include <pthread.h>

extern "C" void *display_worker_thread(void *arg)
{
    const int chnId = (int)(intptr_t)arg;
    DisplayQueue &dq = g_display_queues[chnId];

    while (g_pCtrl && g_pCtrl->isRunning)
    {
        /* ---- 等待新帧（条件变量阻塞，零 CPU 占用）---- */
        DisplayTask task;
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
         * overlay 在 display_commit_frame 内读取共享 last_results，
         * 按 result_age_ms 做卡尔曼速度外推绘制框（实时平滑预览）。*/
        display_commit_frame(task.chnId, dq.pool.front_buf(), task.srcFmt, task.srcWidth, task.srcHeight,
                              task.srcHStride, task.srcVStride);
    }

    return nullptr;
}
