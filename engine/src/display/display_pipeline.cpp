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
 *   display_commit_frame 按检测结果的实际坐标绘制框。
 *   logic/上报路径用严格同帧匹配的数据。
 *
 * 帧池设计要点（见 DisplayFramePool 注释）：
 *   持 DisplayQueue::mtx 的临界区只做整数级槽交换（≈10 ns），
 *   3 MB 的 memcpy 已由生产者在锁外提前完成。
 *   front 槽由本线程独占，display_commit_frame 期间无需持锁。
 */

#include "display_pipeline.h"
#include "pipeline/pipeline_internal.h"
#include "rtsp/rtsp_streamer.h"
#include <cerrno>
#include <ctime>
#include <pthread.h>

extern "C" void *display_worker_thread(void *arg)
{
    const int chnId = (int)(intptr_t)arg;
    DisplayQueue &dq = g_display_queues[chnId];
    bool had_performance_display = false;

    while (g_pCtrl && g_pCtrl->isRunning)
    {
        /* 新帧到达立即处理；停帧时每 250ms 更新性能文字。 */
        DisplayTask task;
        bool reset_fps = false;
        bool has_frame = false;
        {
            timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 250000000;
            if (deadline.tv_nsec >= 1000000000)
            {
                ++deadline.tv_sec;
                deadline.tv_nsec -= 1000000000;
            }
            pthread_mutex_lock(&dq.mtx);
            while (!dq.has_task && g_pCtrl && g_pCtrl->isRunning)
            {
                if (pthread_cond_timedwait(&dq.cv, &dq.mtx, &deadline) == ETIMEDOUT)
                    break;
            }
            if (!g_pCtrl || !g_pCtrl->isRunning)
            {
                pthread_mutex_unlock(&dq.mtx);
                break;
            }
            has_frame = dq.has_task;
            if (has_frame)
            {
                task = dq.task;
                reset_fps = dq.reset_fps_pending;
                dq.reset_fps_pending = false;
                dq.pool.swap_front_if_dirty();
                dq.has_task = 0;
            }
            pthread_mutex_unlock(&dq.mtx);
        }

        const bool show_perf = app_ctrl_get_performance_display();
        if (!has_frame)
        {
            const bool consumer_active = app_ctrl_get_enable_disp() ||
                (app_ctrl_get_enable_rtsp() && rtsp_streamer_has_active_client());
            if (consumer_active && (show_perf || had_performance_display))
                display_refresh_channel_overlays(chnId);
            had_performance_display = show_perf;
            continue;
        }
        had_performance_display = show_perf;
        if (reset_fps)
            g_pCtrl->channels_state[chnId].preview_rate.reset();

        /* ---- RGA 缩放 + render_overlays + 写 framebuffer ----
         * front_buf() 无需持锁：
         *   生产者只写 back 槽（back_idx ≠ front_idx 始终成立），
         *   front 槽由本线程独占直到下次 swap_front_if_dirty。
         *
         * overlay 在 display_commit_frame 内读取共享 last_results，
         * 按检测结果的实际坐标绘制框。*/
        display_commit_frame(task.chnId, dq.pool.front_buf(), task.srcFmt, task.srcWidth, task.srcHeight,
                              task.srcHStride, task.srcVStride);
    }

    return nullptr;
}
