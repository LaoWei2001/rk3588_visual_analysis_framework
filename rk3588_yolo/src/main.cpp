/**
 * @file main.cpp
 * @brief 多路 RTSP YOLO 推理主入口 — C/pthread 风格重构
 *
 * === 线程一览 (全部清晰可见) ===
 *
 * 1. config_monitor_thread  — 配置文件热加载监控 (main 直接 pthread_create)
 * 2. fd_monitor_thread      — fd 使用量监控 (main 直接 pthread_create)
 * 3. capture_bus_thread[N]  — GStreamer bus 监听 + 重连 (DecChannel::init 内部创建, 底层)
 * 4. display_worker[N]      — 异步显示 RGA + framebuffer (main 直接 pthread_create)
 * 5. dispatch_worker[N]     — NPU 结果分发 + channel_logic (main 直接 pthread_create)
 * 6. infer_worker[N]        — NPU 推理 worker (algorithm_init 内部创建, 底层)
 * 7. global_logic[N]        — 跨通道全局逻辑轮询 (global_logic_start_all 内部创建)
 * 8. upload_worker          — Redis 报警异步上传 (alarm_uploader_init 内部创建)
 *
 * === 同步模型 ===
 * mtx (rwlock)    — 保护 config 读/写
 * chn_mtx[i]       — 保护 channels_state[i]
 * cv_config        — config_monitor 定时唤醒/退出信号
 *
 * === 退出流程 ===
 * 信号 → isRunning=0 → 唤醒所有等待线程 → 逆序 join → 释放资源
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>
#include <glib-unix.h>
#include <pthread.h>
#include <algorithm>
#include <chrono>

#include "system.h"
#include "config/config.h"
#include "core/app_ctrl.h"
#include "core/pause_ctrl.h"
#include "capturer/decChannel.h"
#include "analyzer/analyzer.h"
#include "logic/global_logic.h"
#include "player/display.h"
#include "player/rtsp_streamer.h"
#include "uploader/alarm_uploader.h"

/* config_monitor_thread_func — 由 app_ctrl.cpp 导出 (C++ mangling) */
extern "C" void *config_monitor_thread_func(void *arg);

/*======================== 信号处理 ========================*/

/* 信号触发标志: 所有工作线程检查此标志退出 */
static volatile sig_atomic_t g_signal_received = 0;

static void signal_handler(int sig)
{
    (void)sig;
    g_signal_received = 1;
    if (g_pCtrl)
    {
        g_pCtrl->isRunning = 0;
        /* 唤醒 config_monitor (pthread_cond 在 signal handler 中
         * 严格来说不是异步信号安全的, 但 Linux/glibc 实际可行) */
        pthread_cond_broadcast(&g_pCtrl->cv_config);
    }
    pause_ctrl::resume_all();
}

/* SIGUSR1: 暂停/恢复切换 */
static void sigusr1_handler(int sig)
{
    (void)sig;
    pause_ctrl::toggle();
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
        printf("[Main] RLIMIT_NOFILE already %lu (>= %lu), keep\n",
               (unsigned long)rl.rlim_cur, (unsigned long)want);
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
    printf("[Main] RLIMIT_NOFILE raised: soft %lu -> %lu, hard %lu -> %lu\n",
           (unsigned long)rl.rlim_cur, (unsigned long)nrl.rlim_cur,
           (unsigned long)rl.rlim_max, (unsigned long)nrl.rlim_max);
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
    while (g_pCtrl && g_pCtrl->isRunning && !g_pCtrl->fd_monitor_exit)
    {
        /* 60 秒超时等待 */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 60;

        pthread_mutex_lock(&g_pCtrl->cv_config_mtx);
        pthread_cond_timedwait(&g_pCtrl->cv_config, &g_pCtrl->cv_config_mtx, &ts);
        int running = g_pCtrl->isRunning && !g_pCtrl->fd_monitor_exit;
        pthread_mutex_unlock(&g_pCtrl->cv_config_mtx);

        if (!running)
            break;

        int show = 0;
        if (g_pCtrl)
        {
            pthread_rwlock_rdlock(&g_pCtrl->mtx);
            show = g_pCtrl->config.performance_display;
            pthread_rwlock_unlock(&g_pCtrl->mtx);
        }
        if (!show)
            continue;

        int fd_count = count_self_fds();
        struct rlimit rl;
        getrlimit(RLIMIT_NOFILE, &rl);
        printf("[Perf] fd_in_use=%d soft_limit=%lu hard_limit=%lu\n",
               fd_count, (unsigned long)rl.rlim_cur, (unsigned long)rl.rlim_max);
    }
    return nullptr;
}

/*======================== 线程句柄数组 ========================*/
/* display/dispatch 线程数在运行时确定, 动态分配 */
static pthread_t *g_display_tids = nullptr;
static int g_display_thread_count = 0;
static pthread_t *g_dispatch_tids = nullptr;
static int g_dispatch_thread_count = 0;

/*======================== main ========================*/
int main(int argc, char **argv)
{
    /* -------- 0. 抬高 fd 上限 -------- */
    raise_fd_limit_or_warn();

    /* -------- 1. 配置加载 -------- */
    const char *cfgPath = (argc > 1) ? argv[1] : "./assets/config.json";
    if (app_ctrl_init(cfgPath) != 0)
    {
        fprintf(stderr, "[FATAL] app_ctrl_init failed\n");
        return -1;
    }

    /* -------- 2. GStreamer -------- */
    gst_init(&argc, &argv);

    if (app_ctrl_get_chn_nums() <= 0)
    {
        fprintf(stderr, "[FATAL] no streams configured\n");
        app_ctrl_deinit();
        return -1;
    }

    /* -------- 3. 信号处理 (sigaction — Haikang 风格) -------- */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = signal_handler;
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);

        struct sigaction sa_usr;
        memset(&sa_usr, 0, sizeof(sa_usr));
        sa_usr.sa_handler = sigusr1_handler;
        sa_usr.sa_flags = 0;
        sigaction(SIGUSR1, &sa_usr, nullptr);

        /* 忽略 SIGPIPE (防止 Redis 断连时程序崩溃) */
        struct sigaction sa_pipe;
        memset(&sa_pipe, 0, sizeof(sa_pipe));
        sa_pipe.sa_handler = SIG_IGN;
        sigaction(SIGPIPE, &sa_pipe, nullptr);
    }

    /* -------- 4. 暂停键初始化 -------- */
    if (g_pCtrl->config.enable_pause_key && g_pCtrl->config.enable_display)
    {
        pause_ctrl::init(true);
        printf("[Main] Pause key enabled (SPACE / kill -USR1 %d)\n", getpid());
    }
    else
        pause_ctrl::init(false);

    /* -------- 5. 显示缓冲区 --------
     * GTK 显示和 RTSP 推流共用同一张拼接大图 g_disp, 任一开启都需要分配并合成。
     * 这样无显示器(enable_display=false)、仅 enable_rtsp=true 时也能对外推流。 */
    g_pCtrl->dispDesc = {"rtsp_yolo_grid", 0, 0, app_ctrl_get_disp_width(), app_ctrl_get_disp_height()};
    if (app_ctrl_get_enable_disp() || app_ctrl_get_enable_rtsp())
    {
        g_pCtrl->pDispBuffer = dispBufferMap(&g_pCtrl->dispDesc);
        if (!g_pCtrl->pDispBuffer || !(*g_pCtrl->pDispBuffer))
        {
            fprintf(stderr, "[FATAL] alloc display buffer failed\n");
            app_ctrl_deinit();
            return -2;
        }
    }

    /* -------- 6. 分析器初始化 (数据结构, 不创建线程) -------- */
    if (analyzer_init() != 0)
    {
        fprintf(stderr, "[FATAL] analyzer init failed\n");
        app_ctrl_deinit();
        return -3;
    }

    /* ==================================================================
     * 7. 线程创建区 — 所有线程在此清晰可见
     * ================================================================== */

    /* ---- 7a. 配置热加载监控线程 ---- */
    if (pthread_create(&g_pCtrl->config_monitor_tid, nullptr,
                       config_monitor_thread_func, nullptr) != 0)
    {
        fprintf(stderr, "[FATAL] pthread_create config_monitor failed\n");
        analyzer_deinit();
        app_ctrl_deinit();
        return -4;
    }
    printf("[Main] config_monitor_thread created (tid=%lu)\n",
           (unsigned long)g_pCtrl->config_monitor_tid);

    /* ---- 7b. fd 监控线程 ---- */
    if (pthread_create(&g_pCtrl->fd_monitor_tid, nullptr,
                       fd_monitor_thread_func, nullptr) != 0)
    {
        fprintf(stderr, "[WARNING] pthread_create fd_monitor failed, continuing\n");
        g_pCtrl->fd_monitor_exit = 1;
    }
    else
        printf("[Main] fd_monitor_thread created (tid=%lu)\n",
               (unsigned long)g_pCtrl->fd_monitor_tid);

    /* ---- 7c. 采集器 + bus 监听线程 (底层, DecChannel 内部 pthread_create) ----
     * busListen 线程在 DecChannel::init() → createVideoDecChannel() 中创建,
     * 负责 GStreamer bus 消息监听 + 断流重连. 每个唯一 RTSP 源一个线程. */
    g_pCtrl->capturer_count = 0;
    for (int i = 0; i < app_ctrl_get_chn_nums(); ++i)
    {
        const auto &chCfg = g_pCtrl->config.channels[i];
        SrcCfg_t srcCfg;
        srcCfg.srcType = config_utils::normalize_src_type(chCfg.stream);
        srcCfg.location = config_utils::resolve_stream_location(chCfg.stream, srcCfg.srcType);
        srcCfg.videoEncType = chCfg.stream.video_enc.empty() ? "h264" : config_utils::to_lower_copy(chCfg.stream.video_enc);
        srcCfg.loop = chCfg.stream.loop;

        if (srcCfg.location.empty())
        {
            fprintf(stderr, "[Main] channel %d has empty stream location (src_type=%s)\n",
                    i, srcCfg.srcType.c_str());
            continue;
        }

        /* 检查是否可共享已有采集器 */
        int shared = 0;
        for (int j = 0; j < i; ++j)
        {
            if (!g_pCtrl->capturers[j])
                continue;
            const auto &otherCfg = g_pCtrl->config.channels[j];
            auto otherSrcType = config_utils::normalize_src_type(otherCfg.stream);
            auto otherLocation = config_utils::resolve_stream_location(otherCfg.stream, otherSrcType);
            if (srcCfg.srcType == otherSrcType && srcCfg.location == otherLocation)
            {
                fprintf(stderr, "[Main] channel %d shares stream (%s: %s) with channel %d\n",
                        i, srcCfg.srcType.c_str(), srcCfg.location.c_str(), j);
                g_pCtrl->capturers[j]->addTargetChannel(i);
                shared = 1;
                break;
            }
        }
        if (shared)
            continue;

        DecChannel *ch = new DecChannel(i, srcCfg);
        if (!ch)
        {
            fprintf(stderr, "[Main] channel %d creation failed\n", i);
            continue;
        }

        int ret = ch->init();
        if (ret != 0)
        {
            fprintf(stderr, "[Main] channel %d init failed (code=%d), url=%s\n",
                    i, ret, chCfg.stream.url.c_str());
            delete ch;
            continue;
        }
        g_pCtrl->capturers[i] = ch;
        g_pCtrl->capturer_count++;
        printf("[Main] capture_bus_thread[ch%d] created (via DecChannel::init)\n", i);
    }

    /* ---- 7d. 异步显示线程 (每通道一个) ---- */
    g_display_thread_count = analyzer_get_display_thread_count();
    if (g_display_thread_count > 0)
    {
        g_display_tids = new pthread_t[g_display_thread_count];
        for (int i = 0; i < g_display_thread_count; ++i)
        {
            int chnId = analyzer_get_display_chn_id(i);
            int ret = pthread_create(&g_display_tids[i], nullptr,
                                     display_worker_thread, (void *)(intptr_t)chnId);
            if (ret != 0)
                fprintf(stderr, "[Main] pthread_create display_worker[ch%d] failed: %s\n",
                        chnId, strerror(ret));
            else
                printf("[Main] display_worker[ch%d] created (tid=%lu)\n",
                       chnId, (unsigned long)g_display_tids[i]);
        }
    }

    /* ---- 7e. 结果分发线程 (每推理通道一个) ---- */
    g_dispatch_thread_count = analyzer_get_dispatch_thread_count();
    if (g_dispatch_thread_count > 0)
    {
        g_dispatch_tids = new pthread_t[g_dispatch_thread_count];
        for (int i = 0; i < g_dispatch_thread_count; ++i)
        {
            int chnId = analyzer_get_dispatch_chn_id(i);
            int ret = pthread_create(&g_dispatch_tids[i], nullptr,
                                     dispatch_worker_thread, (void *)(intptr_t)chnId);
            if (ret != 0)
                fprintf(stderr, "[Main] pthread_create dispatch_worker[ch%d] failed: %s\n",
                        chnId, strerror(ret));
            else
                printf("[Main] dispatch_worker[ch%d] created (tid=%lu)\n",
                       chnId, (unsigned long)g_dispatch_tids[i]);
        }
    }

    /* ---- 7f. 推理 worker 线程 (底层, algorithm_init 内部 pthread_create) ----
     * 每个模型实例一个 worker 线程.
     * 线程数 = sum(每个推理通道的 threads 参数), 通常 = 推理通道数 * 1. */
    printf("[Main] infer_workers created by algorithm_init (pthread, see algoProcess.cpp)\n");

    /* ---- 7g. 全局逻辑线程 (global_logic_start_all 内部创建) ----
     * 每个 GlobalLogicConfig 实例一个独立线程.
     * 已在 analyzer_init → global_logic_start_all 中创建. */
    printf("[Main] global_logic threads created by global_logic_start_all (%d instance(s))\n",
           global_logic_get_instance_count());

    /* ---- 7h. Redis 报警上传线程 ---- */
    if (!alarm_uploader_init("127.0.0.1", 6379))
        fprintf(stderr, "[Main] WARNING: Redis not reachable at startup, upload worker will retry\n");
    printf("[Main] upload_worker created (via alarm_uploader_init)\n");

    /* ---- 7i. RTSP 推流服务 (enable_rtsp 时启动) ----
     * 内部起独立 GMainContext + loop 线程 + feeder 线程, 推送与显示屏一致的拼接画面;
     * 与 GTK 显示互不干扰, 无显示器时也能工作. 未启用则为空操作. */
    if (rtsp_streamer_init() != 0)
        fprintf(stderr, "[Main] WARNING: RTSP streamer init failed, continuing without RTSP\n");

    printf("\n[Main] === All threads started. Entering main loop. ===\n\n");

    /* ==================================================================
     * 8. 主循环
     * ================================================================== */
    if (app_ctrl_get_enable_disp())
        display(&g_pCtrl->dispDesc); /* 阻塞直到窗口关闭 */
    else
    {
        while (g_pCtrl->isRunning && !g_signal_received)
            usleep(500 * 1000);
    }

    printf("\n[Main] === Exiting. Joining all threads... ===\n");

    /* ==================================================================
     * 9. 逆序退出 — join 所有线程 → 释放资源
     * ================================================================== */

    /* 步骤 0: 停止 RTSP 推流 (先停其 feeder/loop 线程, 之后才拆显示缓冲/通道) */
    rtsp_streamer_deinit();

    /* 步骤 0b: 解除暂停状态 */
    pause_ctrl::resume_all();

    /* 步骤 1: 通知所有线程停止 */
    if (g_pCtrl)
    {
        g_pCtrl->isRunning = 0;
        g_pCtrl->config_monitor_exit = 1;
        g_pCtrl->fd_monitor_exit = 1;
        g_pCtrl->upload_worker_exit = 1;
        g_pCtrl->disp_thread_exit = 1;
        pthread_cond_broadcast(&g_pCtrl->cv_config);
    }
    g_signal_received = 1;

    /* 步骤 2: 停止采集器 bus 线程 */
    if (g_pCtrl)
    {
        for (int i = 0; i < APP_CTRL_MAX_CAPTURERS; ++i)
        {
            if (g_pCtrl->capturers[i])
            {
                g_pCtrl->capturers[i]->stop();
                delete g_pCtrl->capturers[i];
                g_pCtrl->capturers[i] = nullptr;
            }
        }
    }

    /* 步骤 3: 停推理引擎 → join dispatch 线程 */
    analyzer_deinit(); /* stop infer workers, global_logic, trackers, channel_logic */
    printf("[Main] Joining %d dispatch thread(s)...\n", g_dispatch_thread_count);
    for (int i = 0; i < g_dispatch_thread_count; ++i)
        pthread_join(g_dispatch_tids[i], nullptr);
    delete[] g_dispatch_tids;
    g_dispatch_tids = nullptr;

    /* 步骤 4: join display 线程 (在销毁其队列同步原语之前) */
    printf("[Main] Joining %d display thread(s)...\n", g_display_thread_count);
    analyzer_wake_display_threads(); /* 唤醒所有阻塞在 pthread_cond_wait 的 display 线程 */
    for (int i = 0; i < g_display_thread_count; ++i)
        pthread_join(g_display_tids[i], nullptr);
    delete[] g_display_tids;
    g_display_tids = nullptr;

    /* 步骤 5: 销毁 display 队列同步原语 (display 线程已退出) */
    analyzer_destroy_display_queues();

    /* 步骤 6: 停止上传线程 */
    alarm_uploader_deinit();

    /* 步骤 7: join fd_monitor */
    if (g_pCtrl && !g_pCtrl->fd_monitor_exit)
    {
        g_pCtrl->fd_monitor_exit = 1;
        pthread_cond_broadcast(&g_pCtrl->cv_config);
        pthread_join(g_pCtrl->fd_monitor_tid, nullptr);
    }

    /* 步骤 8: join config_monitor */
    if (g_pCtrl && !g_pCtrl->config_monitor_exit)
    {
        g_pCtrl->config_monitor_exit = 1;
        pthread_cond_broadcast(&g_pCtrl->cv_config);
        pthread_join(g_pCtrl->config_monitor_tid, nullptr);
    }

    /* 步骤 9: 销毁控制块 */
    app_ctrl_deinit();

    printf("[Main] Clean exit.\n");
    return 0;
}
