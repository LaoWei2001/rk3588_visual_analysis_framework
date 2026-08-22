/*
 * Copyright (C) 2026, Sunny_Wei, all rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file main.cpp
 * @brief 多路 RTSP YOLO 推理主入口
 *
 * === 线程一览 (全部清晰可见) ===
 *
 * 1. config_monitor_thread  — 配置文件热加载监控 (main 直接 pthread_create)
 * 2. fd_monitor_thread      — fd 使用量监控 (main 直接 pthread_create)
 * 3. capture_bus_thread[N]  — GStreamer bus 监听 + 重连 (DecChannel::init 内部创建, 底层)
 * 4. display_worker[N]      — 异步显示 RGA + framebuffer (main 直接 pthread_create)
 * 5. dispatch_worker[N]     — NPU 结果分发 + channel_logic (main 直接 pthread_create)
 * 6. infer_worker[N]        — NPU 推理 worker (inference_init 内部创建, 底层)
 * 7. global_logic[N]        — 跨通道全局逻辑轮询 (global_logic_start_all 内部创建)
 * 8. event_image_worker     — 事件图片异步渲染/编码并更新媒体状态 (首次图片事件时创建)
 * 9. event_video_worker     — 报警前后片段异步编码 (首次启用录像时创建)
 *
 * === 同步模型 ===
 * mtx (rwlock)    — 保护 config 读/写
 * chn_mtx[i]       — 保护 channels_state[i]
 * cv_config        — config_monitor 定时唤醒/退出信号
 *
 * === 退出流程 ===
 * 信号 → isRunning=0 → 唤醒所有等待线程 → 逆序 join → 释放资源
 */

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <glib-unix.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

#include "event/event_report.h"
#include "pipeline/pipeline_runtime.h"
#include "capturer/decChannel.h"
#include "config/config.h"
#include "control/logic_control.h"
#include "runtime/app_ctrl.h"
#include "runtime/pause_ctrl.h"
#include "runtime/process_signals.h"
#include "logic/core/channel_logic.h"
#include "logic/core/global_logic.h"
#include "display/display.h"
#include "display/display_pipeline.h"
#include "rtsp/rtsp_streamer.h"
#include "recorder/event_video_recorder.h"
#include "common/logging.h"

/* config_monitor_thread_func — 由 app_ctrl.cpp 导出 (C++ mangling) */
extern "C" void *config_monitor_thread_func(void *arg);

/*======================== 信号处理 ========================*/

/* 信号触发标志: 所有工作线程检查此标志退出 */
volatile sig_atomic_t g_shutdown_signal = 0;
volatile sig_atomic_t g_pause_toggle_signal = 0;

static void signal_handler(int sig)
{
    (void)sig;
    g_shutdown_signal = 1;
}

/* SIGUSR1: 暂停/恢复切换 */
static void sigusr1_handler(int sig)
{
    (void)sig;
    g_pause_toggle_signal = 1;
}

/*======================== 资源上限 + fd 监控 ========================*/

static void raise_fd_limit_or_warn(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
    {
        fprintf(stderr, "[Main] getrlimit(RLIMIT_NOFILE) failed: %s\n", strerror(errno));
        return;
    }
    rlim_t want = 65536;
    if (rl.rlim_cur >= want)
    {
        printf("[Main] RLIMIT_NOFILE already %lu (>= %lu), keep\n", (unsigned long)rl.rlim_cur, (unsigned long)want);
        return;
    }
    struct rlimit nrl = rl;
    nrl.rlim_cur = std::min<rlim_t>(want, rl.rlim_max == RLIM_INFINITY ? want : rl.rlim_max);
    if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < want)
        nrl.rlim_max = rl.rlim_max;
    else
        nrl.rlim_max = want;

    if (setrlimit(RLIMIT_NOFILE, &nrl) != 0)
    {
        nrl.rlim_max = rl.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &nrl) != 0)
        {
            fprintf(stderr, "[Main] setrlimit(RLIMIT_NOFILE) failed: %s\n", strerror(errno));
            return;
        }
    }
    printf("[Main] RLIMIT_NOFILE raised: soft %lu -> %lu, hard %lu -> %lu\n", (unsigned long)rl.rlim_cur,
           (unsigned long)nrl.rlim_cur, (unsigned long)rl.rlim_max, (unsigned long)nrl.rlim_max);
}

static int count_self_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr)
        if (de->d_name[0] != '.')
            ++n;
    closedir(d);
    return n > 0 ? n - 1 : n;
}

/* fd 监控线程 (pthread 入口) */
static void *fd_monitor_thread_func(void *arg)
{
    (void)arg;
    while (g_pCtrl && g_pCtrl->isRunning.load() && !g_pCtrl->fd_monitor_exit.load())
    {
        /* 60 秒超时等待 */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 60;

        pthread_mutex_lock(&g_pCtrl->cv_config_mtx);
        pthread_cond_timedwait(&g_pCtrl->cv_config, &g_pCtrl->cv_config_mtx, &ts);
        const bool running = g_pCtrl->isRunning.load() && !g_pCtrl->fd_monitor_exit.load();
        pthread_mutex_unlock(&g_pCtrl->cv_config_mtx);

        if (!running)
            break;

        int show = 0;
        if (g_pCtrl)
            show = app_ctrl_get_performance_display();
        if (!show)
            continue;

        int fd_count = count_self_fds();
        struct rlimit rl;
        getrlimit(RLIMIT_NOFILE, &rl);
        printf("[Perf] fd_in_use=%d soft_limit=%lu hard_limit=%lu\n", fd_count, (unsigned long)rl.rlim_cur,
               (unsigned long)rl.rlim_max);
    }
    return nullptr;
}

/*======================== main ========================*/
int main(int argc, char **argv)
{
    /* Web 控制台通过 PIPE 捕获日志。stdout 连到管道时 libc 默认使用块缓冲，
     * 冷启动/重连日志可能长时间不显示；统一改为逐行刷新。 */
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    if (argc == 2 && strcmp(argv[1], "--list-logics") == 0)
    {
        for (const auto &name : channel_logic_names())
            printf("%s\n", name.c_str());
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--list-global-logics") == 0)
    {
        for (const auto &name : global_logic_names())
            printf("%s\n", name.c_str());
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "--validate-config") == 0)
    {
        AppConfig config;
        if (!load_config(argv[2], config))
            return 1;
        printf("[Config] validation passed: %s (%zu channels)\n", argv[2], config.channels.size());
        return 0;
    }

    const char *cfgPath = (argc > 1) ? argv[1] : "./assets/config_sop.json";
    int exit_code = 0;
    bool ctrl_initialized = false;
    bool pipeline_initialized = false;
    bool config_monitor_started = false;
    bool fd_monitor_started = false;
    std::vector<pthread_t> display_tids;
    std::vector<pthread_t> dispatch_tids;
    std::shared_ptr<const AppRuntimeSnapshot> startup_runtime;

    raise_fd_limit_or_warn();
    if (app_ctrl_init(cfgPath) != 0)
    {
        fprintf(stderr, "[FATAL] app_ctrl_init failed\n");
        return -1;
    }
    ctrl_initialized = true;
    gst_init(&argc, &argv);

    if (app_ctrl_get_chn_nums() <= 0)
    {
        fprintf(stderr, "[FATAL] no streams configured\n");
        exit_code = -1;
        goto cleanup;
    }

    {
        struct sigaction sa
        {
        };
        sa.sa_handler = signal_handler;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        struct sigaction sa_usr
        {
        };
        sa_usr.sa_handler = sigusr1_handler;
        sigaction(SIGUSR1, &sa_usr, nullptr);
        struct sigaction sa_pipe
        {
        };
        sa_pipe.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &sa_pipe, nullptr);
    }

    if (g_pCtrl->config.enable_pause_key && g_pCtrl->config.enable_display)
    {
        pause_ctrl::init(true);
        printf("[Main] Pause key enabled (SPACE / kill -USR1 %d)\n", getpid());
    }
    else
        pause_ctrl::init(false);

    g_pCtrl->dispDesc = {"rtsp_yolo_grid", 0, 0, app_ctrl_get_disp_width(), app_ctrl_get_disp_height()};
    if (app_ctrl_get_enable_disp() || app_ctrl_get_enable_rtsp())
    {
        g_pCtrl->pDispBuffer = dispBufferMap(&g_pCtrl->dispDesc);
        if (!g_pCtrl->pDispBuffer || !*g_pCtrl->pDispBuffer)
        {
            fprintf(stderr, "[FATAL] alloc display buffer failed\n");
            exit_code = -2;
            goto cleanup;
        }
    }

    if (pipeline_init() != 0)
    {
        fprintf(stderr, "[FATAL] pipeline init failed\n");
        exit_code = -3;
        goto cleanup;
    }
    pipeline_initialized = true;

    if (logic_control_init() != 0)
        fprintf(stderr, "[Main] WARNING: channel control init failed, web action buttons disabled\n");

    g_pCtrl->capturer_count = 0;
    startup_runtime = app_ctrl_get_runtime_snapshot();
    for (int config_index = 0; config_index < static_cast<int>(startup_runtime->config.channels.size()); ++config_index)
    {
        const auto &chCfg = startup_runtime->config.channels[config_index];
        const int channel_id = chCfg.id;
        SrcCfg_t srcCfg;
        srcCfg.srcType = config_utils::normalize_src_type(chCfg.stream);
        srcCfg.location = config_utils::resolve_stream_location(chCfg.stream, srcCfg.srcType);
        srcCfg.videoEncType =
            chCfg.stream.video_enc.empty() ? "h264" : config_utils::to_lower_copy(chCfg.stream.video_enc);
        srcCfg.loop = chCfg.stream.loop;
        srcCfg.usb_width = chCfg.stream.usb_width;
        srcCfg.usb_height = chCfg.stream.usb_height;
        srcCfg.usb_fps =
            chCfg.playback_fps > 0 ? chCfg.playback_fps : (chCfg.max_fps > 0 ? chCfg.max_fps : app_ctrl_get_max_fps());
        if (srcCfg.location.empty())
        {
            fprintf(stderr, "[Main] channel %d has empty stream location (src_type=%s)\n", channel_id,
                    srcCfg.srcType.c_str());
            continue;
        }

        bool shared = false;
        for (int other_index = 0; other_index < config_index; ++other_index)
        {
            const auto &otherCfg = startup_runtime->config.channels[other_index];
            const int other_id = otherCfg.id;
            if (!g_pCtrl->capturers[other_id])
                continue;
            const auto otherSrcType = config_utils::normalize_src_type(otherCfg.stream);
            const auto otherLocation = config_utils::resolve_stream_location(otherCfg.stream, otherSrcType);
            if (srcCfg.srcType == otherSrcType && srcCfg.location == otherLocation)
            {
                printf("[Main] channel %d shares stream with channel %d\n", channel_id, other_id);
                g_pCtrl->capturers[other_id]->addTargetChannel(channel_id);
                shared = true;
                break;
            }
        }
        if (shared)
            continue;

        DecChannel *ch = new DecChannel(channel_id, srcCfg);
        const int ret = ch ? ch->init() : -1;
        if (ret != 0)
        {
            fprintf(stderr, "[Main] channel %d init failed (code=%d), url=%s\n", channel_id, ret,
                    chCfg.stream.url.c_str());
            delete ch;
            continue;
        }
        g_pCtrl->capturers[channel_id] = ch;
        ++g_pCtrl->capturer_count;
        printf("[Main] capture_bus_thread[channel=%d] created\n", channel_id);
    }

    for (int i = 0; i < pipeline_get_display_thread_count(); ++i)
    {
        const int channel_id = pipeline_get_display_chn_id(i);
        pthread_t tid{};
        const int ret = pthread_create(&tid, nullptr, display_worker_thread, (void *)(intptr_t)channel_id);
        if (ret != 0)
            fprintf(stderr, "[Main] pthread_create display_worker[channel=%d] failed: %s\n", channel_id, strerror(ret));
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
            fprintf(stderr, "[Main] pthread_create dispatch_worker[channel=%d] failed: %s\n", channel_id,
                    strerror(ret));
        else
        {
            dispatch_tids.push_back(tid);
            printf("[Main] dispatch_worker[channel=%d] created (tid=%lu)\n", channel_id, (unsigned long)tid);
        }
    }

    printf("[Main] infer workers managed by inference_init; global logic instances=%d\n",
           global_logic_get_instance_count());
    if (rtsp_streamer_init() != 0)
    {
        fprintf(stderr, "[Main] FATAL: RTSP streamer startup failed\n");
        exit_code = -4;
        goto cleanup;
    }

    /* 所有依赖固定拓扑的采集/显示/分发线程就绪后才启动热更新。
     * 避免启动阶段 config_monitor 与 main 同时改 capturers/config。 */
    if (pthread_create(&g_pCtrl->config_monitor_tid, nullptr, config_monitor_thread_func, nullptr) != 0)
    {
        fprintf(stderr, "[FATAL] pthread_create config_monitor failed\n");
        exit_code = -4;
        goto cleanup;
    }
    config_monitor_started = true;
    printf("[Main] config_monitor_thread created (tid=%lu)\n", (unsigned long)g_pCtrl->config_monitor_tid);

    if (pthread_create(&g_pCtrl->fd_monitor_tid, nullptr, fd_monitor_thread_func, nullptr) != 0)
        fprintf(stderr, "[WARNING] pthread_create fd_monitor failed, continuing\n");
    else
    {
        fd_monitor_started = true;
        printf("[Main] fd_monitor_thread created (tid=%lu)\n", (unsigned long)g_pCtrl->fd_monitor_tid);
    }

    printf("\n[Main] === All threads started. Entering main loop. ===\n\n");
    if (app_ctrl_get_enable_disp())
        display(&g_pCtrl->dispDesc);
    else
    {
        while (g_pCtrl->isRunning.load() && !g_shutdown_signal)
        {
            if (g_pause_toggle_signal)
            {
                g_pause_toggle_signal = 0;
                pause_ctrl::toggle();
            }
            usleep(100 * 1000);
        }
    }

cleanup:
    printf("\n[Main] === Stopping and joining all threads... ===\n");
    rtsp_streamer_deinit();
    if (ctrl_initialized)
        app_ctrl_request_stop();
    pause_ctrl::resume_all();
    logic_control_deinit();

    /* 配置线程可能重建 capturer，必须先于 capturer 销毁完成 join。 */
    if (fd_monitor_started)
        pthread_join(g_pCtrl->fd_monitor_tid, nullptr);
    if (config_monitor_started)
        pthread_join(g_pCtrl->config_monitor_tid, nullptr);

    /* 热重载线程退出后再停止算法，避免它在 shutdown 中途重启模型 worker。 */
    if (pipeline_initialized)
        pipeline_request_stop();

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

    /* 先回收仍可能触发 logic/告警的线程，再关闭告警与录像消费者。 */
    for (pthread_t tid : dispatch_tids)
        pthread_join(tid, nullptr);
    for (pthread_t tid : display_tids)
        pthread_join(tid, nullptr);

    if (pipeline_initialized)
        pipeline_deinit();

    /* 先排空事件持久化队列，其中可能还会向录像器提交最后一批任务。 */
    event_report_deinit();
    event_video_recorder_deinit();

    if (pipeline_initialized)
        pipeline_destroy_display_queues();
    dispBufferUnmap();
    if (ctrl_initialized)
        app_ctrl_deinit();

    printf("[Main] Clean exit (code=%d).\n", exit_code);
    return exit_code;
}
