#include "video_runtime_internal.h"

#include "application/control/file_playback_control.h"
#include "application/control/logic_control.h"
#include "application/runtime/app_ctrl.h"
#include "application/runtime/runtime_status.h"
#include "logic/global_logic.h"
#include "media/capture/decode_channel.h"
#include "media/display/display.h"
#include "media/rtsp/rtsp_streamer.h"
#include "pipeline/display_worker.h"
#include "pipeline/pipeline_runtime.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

extern "C" void *config_monitor_thread_func(void *arg);

namespace vision
{
int VideoRuntime::Impl::start()
{
    runtime_detail::raise_fd_limit_or_warn();
    {
        std::lock_guard<std::mutex> lock(control_mutex);
        if (app_ctrl_init(options.config_path.c_str()) != 0)
        {
            error = "无法加载视频配置：" + options.config_path;
            fprintf(stderr, "[FATAL] app_ctrl_init failed\n");
            return -1;
        }
        ctrl_initialized = true;
    }
    if (stopping())
        return 0;
    gst_init(nullptr, nullptr);

    if (app_ctrl_get_chn_nums() <= 0)
    {
        std::lock_guard<std::mutex> lock(control_mutex);
        error = "配置中没有视频通道";
        fprintf(stderr, "[FATAL] no streams configured\n");
        return -1;
    }

    g_pCtrl->dispDesc = {"rtsp_yolo_grid", 0, 0, app_ctrl_get_disp_width(), app_ctrl_get_disp_height()};
    if (app_ctrl_get_enable_disp() || app_ctrl_get_enable_rtsp())
    {
        g_pCtrl->pDispBuffer = dispBufferMap(&g_pCtrl->dispDesc);
        if (!g_pCtrl->pDispBuffer || !*g_pCtrl->pDispBuffer)
        {
            std::lock_guard<std::mutex> lock(control_mutex);
            error = "无法分配显示缓冲区";
            fprintf(stderr, "[FATAL] alloc display buffer failed\n");
            return -2;
        }
    }

    if (pipeline_init() != 0)
    {
        std::lock_guard<std::mutex> lock(control_mutex);
        error = "视频处理初始化失败，请检查配置和启动日志";
        fprintf(stderr, "[FATAL] pipeline init failed\n");
        return -3;
    }
    pipeline_initialized = true;
    if (stopping())
        return 0;

    if (logic_control_init() != 0)
        fprintf(stderr, "[Main] WARNING: channel control init failed, web action buttons disabled\n");

    start_capturers();
    if (stopping())
        return 0;
    return start_workers();
}

void VideoRuntime::Impl::start_capturers()
{
    g_pCtrl->capturer_count = 0;
    const auto startup_runtime = app_ctrl_get_runtime_snapshot();
    if (!startup_runtime)
        throw std::runtime_error("No runtime configuration snapshot");
    struct CaptureGroup
    {
        SrcCfg_t source;
        std::vector<int> channels;
    };
    std::vector<CaptureGroup> groups;
    // 先收齐共享同一视频源的通道，再启动任何 GStreamer 回调。
    for (const auto &chCfg : startup_runtime->config.channels)
    {
        if (!chCfg.enable)
            continue;
        const int channel_id = chCfg.id;
        SrcCfg_t srcCfg;
        srcCfg.srcType = config_utils::normalize_src_type(chCfg.stream);
        srcCfg.location = config_utils::resolve_stream_location(chCfg.stream, srcCfg.srcType);
        srcCfg.videoEncType =
            chCfg.stream.video_enc.empty() ? "h264" : config_utils::to_lower_copy(chCfg.stream.video_enc);
        srcCfg.loop = chCfg.stream.loop;
        srcCfg.usb_width = chCfg.stream.usb_width;
        srcCfg.usb_height = chCfg.stream.usb_height;
        if (srcCfg.location.empty())
        {
            app_ctrl_capture_status(channel_id, VideoChannelState::Failed, "视频源地址为空");
            fprintf(stderr, "[Main] channel %d has empty stream location (src_type=%s)\n", channel_id,
                    srcCfg.srcType.c_str());
            continue;
        }

        bool shared = false;
        for (auto &group : groups)
        {
            if (capture_sources_share(group.source, srcCfg))
            {
                group.channels.push_back(channel_id);
                shared = true;
                break;
            }
        }
        if (!shared)
            groups.push_back({std::move(srcCfg), {channel_id}});
    }
    for (const auto &group : groups)
    {
        if (stopping())
            break;
        const int channel_id = group.channels.front();
        std::unique_ptr<DecChannel> ch(new DecChannel(group.channels, group.source));
        const int ret = ch->init();
        if (ret != 0)
        {
            const std::string error = "视频源启动失败，错误码 " + std::to_string(ret);
            for (int id : group.channels)
                app_ctrl_capture_status(id, VideoChannelState::Failed, error.c_str());
            fprintf(stderr, "[Main] channel %d init failed (code=%d), url=%s\n", channel_id, ret,
                    group.source.location.c_str());
            continue;
        }
        g_pCtrl->capturers[channel_id] = ch.release();
        ++g_pCtrl->capturer_count;
        printf("[Main] capture_bus_thread[channel=%d] created\n", channel_id);
    }
}

int VideoRuntime::Impl::start_workers()
{
    // Allocate before pthread_create: allocation failure must not lose a joinable worker.
    display_tids.reserve(pipeline_get_display_thread_count());
    dispatch_tids.reserve(pipeline_get_dispatch_thread_count());
    logic_tids.reserve(pipeline_get_logic_thread_count());
    if (file_playback_control_init() != 0)
        fprintf(stderr, "[Main] WARNING: file playback control init failed, Web progress bars disabled\n");

    for (int i = 0; i < pipeline_get_display_thread_count(); ++i)
    {
        const int channel_id = pipeline_get_display_chn_id(i);
        pthread_t tid{};
        const int ret = pthread_create(&tid, nullptr, display_worker_thread, (void *)(intptr_t)channel_id);
        if (ret != 0)
        {
            fprintf(stderr, "[Main] pthread_create display_worker[channel=%d] failed: %s\n", channel_id, strerror(ret));
            return -4;
        }
        else
        {
            display_tids.push_back(tid);
            printf("[Main] display_worker[channel=%d] created (tid=%lu)\n", channel_id, (unsigned long)tid);
        }
    }

    for (int i = 0; i < pipeline_get_dispatch_thread_count(); ++i)
    {
        const int channel_id = pipeline_get_dispatch_chn_id(i);
        pthread_t tid{};
        const int ret = pthread_create(&tid, nullptr, pipeline_dispatch_worker, (void *)(intptr_t)channel_id);
        if (ret != 0)
        {
            fprintf(stderr, "[Main] pthread_create dispatch_worker[channel=%d] failed: %s\n", channel_id,
                    strerror(ret));
            return -4;
        }
        else
        {
            dispatch_tids.push_back(tid);
            printf("[Main] dispatch_worker[channel=%d] created (tid=%lu)\n", channel_id, (unsigned long)tid);
        }
    }

    for (int i = 0; i < pipeline_get_logic_thread_count(); ++i)
    {
        const int channel_id = pipeline_get_logic_chn_id(i);
        pthread_t tid{};
        const int ret = pthread_create(&tid, nullptr, pipeline_logic_worker, (void *)(intptr_t)channel_id);
        if (ret != 0)
        {
            fprintf(stderr, "[Main] pthread_create logic_worker[channel=%d] failed: %s\n", channel_id, strerror(ret));
            return -4;
        }
        else
        {
            logic_tids.push_back(tid);
            printf("[Main] logic_worker[channel=%d] created (tid=%lu)\n", channel_id, (unsigned long)tid);
        }
    }

    printf("[Main] infer workers managed by inference_init; global logic instances=%d\n",
           global_logic_get_instance_count());
    if (rtsp_streamer_init() != 0)
    {
        fprintf(stderr, "[Main] FATAL: RTSP streamer startup failed\n");
        return -4;
    }

    /* 所有依赖固定拓扑的采集/显示/分发线程就绪后才启动热更新。
     * 避免启动阶段 config_monitor 与启动流程同时改 capturers/config。 */
    if (pthread_create(&g_pCtrl->config_monitor_tid, nullptr, config_monitor_thread_func, nullptr) != 0)
    {
        fprintf(stderr, "[FATAL] pthread_create config_monitor failed\n");
        return -4;
    }
    config_monitor_started = true;
    printf("[Main] config_monitor_thread created (tid=%lu)\n", (unsigned long)g_pCtrl->config_monitor_tid);

    if (pthread_create(&g_pCtrl->fd_monitor_tid, nullptr, runtime_detail::fd_monitor_thread, nullptr) != 0)
        fprintf(stderr, "[WARNING] pthread_create fd_monitor failed, continuing\n");
    else
    {
        fd_monitor_started = true;
        printf("[Main] fd_monitor_thread created (tid=%lu)\n", (unsigned long)g_pCtrl->fd_monitor_tid);
    }

    return 0;
}
} // namespace vision
