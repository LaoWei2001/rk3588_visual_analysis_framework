#include "video_runtime_internal.h"

#include "application/control/file_playback_control.h"
#include "application/control/logic_control.h"
#include "application/runtime/app_ctrl.h"
#include "application/runtime/runtime_status.h"
#include "application/runtime/service_watchdog.h"
#include "application/runtime/shutdown_status.h"
#include "events/event_report.h"
#include "media/capture/decode_channel.h"
#include "media/display/display.h"
#include "media/recorder/event_video_recorder.h"
#include "media/rtsp/rtsp_streamer.h"
#include "pipeline/pipeline_runtime.h"
#include "platform/gpio/gpio_runtime.h"
#include "vision/inference/inference_engine.h"

#include <cstdio>

namespace vision
{
int VideoRuntime::Impl::shutdown(int exit_code)
{
    printf("\n[Main] === Stopping and joining all threads... ===\n");
    inference_request_stop(); // 先唤醒等待者，防止配置线程切模型时阻塞退出。
    service_watchdog::request_shutdown();
    shutdown_status_stage("stop_requested");
    if (ctrl_initialized)
        app_ctrl_request_stop();
    shutdown_status_stage("control_connections");
    logic_control_deinit();
    file_playback_control_deinit();

    shutdown_status_stage("configuration_threads");
    /* 配置线程可能重建 capturer，必须先于 capturer 销毁完成 join。 */
    if (fd_monitor_started)
        pthread_join(g_pCtrl->fd_monitor_tid, nullptr);
    if (config_monitor_started)
        pthread_join(g_pCtrl->config_monitor_tid, nullptr);

    shutdown_status_stage("rtsp_streamer");
    rtsp_streamer_deinit();

    /* 热重载线程退出后再停止算法，避免它在 shutdown 中途重启模型 worker。 */
    if (pipeline_initialized)
        pipeline_request_stop();

    shutdown_status_stage("capture_threads");
    if (g_pCtrl)
    {
        for (int i = 0; i < APP_CTRL_MAX_CAPTURERS; ++i)
        {
            if (!g_pCtrl->capturers[i])
                continue;
            g_pCtrl->capturers[i]->stop();
            delete g_pCtrl->capturers[i];
            g_pCtrl->capturers[i] = nullptr;
        }
        g_pCtrl->capturer_count = 0;
    }

    shutdown_status_stage("business_threads");
    /* 先回收仍可能触发 logic/告警的线程，再关闭告警与录像消费者。 */
    for (pthread_t tid : logic_tids)
        pthread_join(tid, nullptr);
    for (pthread_t tid : dispatch_tids)
        pthread_join(tid, nullptr);
    for (pthread_t tid : display_tids)
        pthread_join(tid, nullptr);

    if (pipeline_initialized)
        pipeline_deinit();

    // Global logic state destructors may still write the final GPIO levels.
    // Destroy channel business states while the control block is also alive.
    if (g_pCtrl)
        for (auto &channel : g_pCtrl->channels_state)
            channel.logic_state.reset();
    gpio_deinit();

    /* 先排空事件持久化队列，其中可能还会向录像器提交最后一批任务。 */
    shutdown_status_stage("event_persistence");
    event_report_deinit();
    shutdown_status_stage("event_video_recordings");
    event_video_recorder_deinit();

    shutdown_status_stage("release_resources");
    if (pipeline_initialized)
        pipeline_destroy_display_queues();
    dispBufferUnmap();
    {
        std::lock_guard<std::mutex> lock(control_mutex);
        if (ctrl_initialized)
        {
            {
                std::lock_guard<std::mutex> failure_lock(g_pCtrl->failure_mutex);
                if (g_pCtrl->worker_failed)
                {
                    error = g_pCtrl->worker_error;
                    if (exit_code == 0)
                        exit_code = 7;
                }
            }
            final_status = app_ctrl_channel_status();
            for (auto &channel : final_status)
                if (channel.state != VideoChannelState::Failed)
                    channel.state = VideoChannelState::Stopped;
            app_ctrl_deinit();
            ctrl_initialized = false;
        }
    }

    service_watchdog::stop();
    {
        const unsigned long failures = event_report_failure_count();
        if (failures && exit_code == 0)
            exit_code = 5;
        if (exit_code != 0)
        {
            std::lock_guard<std::mutex> lock(control_mutex);
            if (error.empty())
                error = "VideoRuntime exited with code " + std::to_string(exit_code);
        }
        shutdown_status_complete(exit_code, failures);
        if (exit_code == 0)
            printf("[Main] Clean exit (code=0).\n");
        else
            fprintf(stderr, "[Main] Cleanup completed with errors (code=%d, evidence_errors=%lu).\n", exit_code,
                    failures);
    }
    return exit_code;
}
} // namespace vision
