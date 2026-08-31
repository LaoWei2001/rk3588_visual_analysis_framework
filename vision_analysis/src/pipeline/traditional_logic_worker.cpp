/**
 * @file traditional_logic_worker.cpp
 * @brief 非 NPU 通道的通用异步逻辑工作线程。
 *
 * appsink 回调只导入一个具有稳定生命周期的 DMA-BUF handle，然后发布到
 * 单槽最新帧队列。所有传统 CV、业务逻辑和 display_canvas() 操作都在本线程执行。
 * 这一约束由框架保证，新增任何通道逻辑都无需再自行解决解码阻塞问题。
 */

#include "pipeline_internal.h"
#include "pipeline_runtime.h"

#include "runtime/pause_ctrl.h"

#include <utility>

namespace
{

bool channel_should_run_traditional_logic(int chn_id, uint64_t frame_steady_ms, uint64_t task_generation)
{
    const auto runtime = app_ctrl_get_runtime_snapshot();
    if (!runtime || runtime->generation != task_generation)
        return false;
    const ChannelConfig *channel_config = app_ctrl_runtime_channel_config(runtime, chn_id);
    if (!channel_config || !channel_config->enable)
        return false;

    bool infer_enabled = config_utils::is_channel_infer_enabled(*channel_config);
    pthread_mutex_lock(&g_pCtrl->chn_mtx[chn_id]);
    const ChannelState &state = g_pCtrl->channels_state[chn_id];
    infer_enabled = infer_enabled && state.infer_runtime_enable;
    const bool online = state.online_state == CH_ONLINE;
    const bool stale_after_reconnect =
        state.online_ts_ms != 0 && frame_steady_ms != 0 && frame_steady_ms <= state.online_ts_ms;
    pthread_mutex_unlock(&g_pCtrl->chn_mtx[chn_id]);
    return online && !stale_after_reconnect && !infer_enabled;
}

} // namespace

void traditional_logic_publish(int chn_id, ChannelRawFrame raw_frame, int64_t frame_seq)
{
    if (chn_id < 0 || chn_id >= MAX_CHANNEL_NUM || !g_dispatch_running.load())
        return;

    const auto runtime = app_ctrl_get_runtime_snapshot();
    TraditionalLogicQueue &queue = g_traditional_logic_queues[chn_id];
    TraditionalLogicTask superseded;
    pthread_mutex_lock(&queue.mtx);
    if (queue.has_task)
        superseded = std::move(queue.task);
    queue.task.raw_frame = std::move(raw_frame);
    queue.task.frame_seq = frame_seq;
    queue.task.runtime_generation = runtime ? runtime->generation : 0;
    queue.has_task = true;
    pthread_mutex_unlock(&queue.mtx);
    pthread_cond_signal(&queue.cv);
    /* superseded 在锁外析构，避免 releasebuffer_handle() 延长生产者临界区。 */
}

extern "C" void *pipeline_logic_worker(void *arg)
{
    const int chn_id = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    if (chn_id < 0 || chn_id >= MAX_CHANNEL_NUM)
        return nullptr;
    TraditionalLogicQueue &queue = g_traditional_logic_queues[chn_id];

    while (g_dispatch_running.load())
    {
        TraditionalLogicTask task;
        pthread_mutex_lock(&queue.mtx);
        while (!queue.has_task && g_dispatch_running.load())
            pthread_cond_wait(&queue.cv, &queue.mtx);
        if (!g_dispatch_running.load())
        {
            pthread_mutex_unlock(&queue.mtx);
            break;
        }
        task = std::move(queue.task);
        queue.has_task = false;
        pthread_mutex_unlock(&queue.mtx);

        if (pause_ctrl::is_paused() || !task.raw_frame.lazy_frame)
            continue;

        /* 与热更新、在线状态切换和同通道 NPU 在途结果串行。
         * 获锁后重新检查推理开关，避免开关翻转后执行旧的传统任务。 */
        pthread_mutex_lock(&g_process_mtx[chn_id]);
        if (channel_should_run_traditional_logic(chn_id, task.raw_frame.frame_steady_ms, task.runtime_generation))
            process_channel_results(chn_id, task.raw_frame, nullptr, task.frame_seq);
        pthread_mutex_unlock(&g_process_mtx[chn_id]);
    }

    return nullptr;
}
