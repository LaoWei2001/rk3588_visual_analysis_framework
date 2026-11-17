#include "runtime_status.h"
#include "app_ctrl.h"
#include "common/pthread_lock.h"

#include <chrono>
#include <cstdio>

std::vector<vision::VideoChannelStatus> app_ctrl_channel_status()
{
    std::vector<vision::VideoChannelStatus> result;
    const auto runtime = app_ctrl_get_runtime_snapshot();
    if (!g_pCtrl || !runtime)
        return result;
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    for (const auto &config : runtime->config.channels)
    {
        if (!config.enable)
            continue;
        vision::VideoChannelStatus status;
        status.channel_id = config.id;
        {
            PthreadLock lock(g_pCtrl->chn_mtx[config.id]);
            const auto &channel = g_pCtrl->channels_state[config.id];
            status.state = channel.capture_state;
            status.received_frames = channel.received_frames;
            status.width = channel.src_w_now;
            status.height = channel.src_h_now;
            if (channel.last_input_steady_ms)
                status.last_frame_age_ms =
                    now >= channel.last_input_steady_ms ? static_cast<int64_t>(now - channel.last_input_steady_ms) : 0;
            status.error = channel.capture_error;
            status.network_rtt_ms = channel.network_rtt_ms;
        }
        result.push_back(std::move(status));
    }
    return result;
}

void app_ctrl_network_rtt(int channel_id, float milliseconds) noexcept
{
    if (!app_ctrl_has_channel(channel_id))
        return;
    PthreadLock lock(g_pCtrl->chn_mtx[channel_id]);
    g_pCtrl->channels_state[channel_id].network_rtt_ms = milliseconds;
}

void app_ctrl_capture_status(int channel_id, vision::VideoChannelState state, const char *error) noexcept
{
    if (!app_ctrl_has_channel(channel_id))
        return;
    PthreadLock lock(g_pCtrl->chn_mtx[channel_id]);
    auto &channel = g_pCtrl->channels_state[channel_id];
    // 已退出的业务线程不能被稍后到达的采集状态覆盖。
    if (channel.capture_state == vision::VideoChannelState::Failed && !g_pCtrl->isRunning.load())
        return;
    channel.capture_state = state;
    std::snprintf(channel.capture_error, sizeof(channel.capture_error), "%s", error ? error : "");
}

void app_ctrl_worker_failed(int channel_id, const char *worker, const char *error) noexcept
{
    if (!g_pCtrl)
        return;
    char message[512];
    std::snprintf(message, sizeof(message), "%s (channel=%d): %s", worker, channel_id, error);
    {
        std::lock_guard<std::mutex> lock(g_pCtrl->failure_mutex);
        if (!g_pCtrl->worker_failed)
            std::snprintf(g_pCtrl->worker_error, sizeof(g_pCtrl->worker_error), "%s", message);
        g_pCtrl->worker_failed = true;
    }
    app_ctrl_request_stop();
    app_ctrl_capture_status(channel_id, vision::VideoChannelState::Failed, message);
    std::fprintf(stderr, "[Runtime] %s\n", message);
}
