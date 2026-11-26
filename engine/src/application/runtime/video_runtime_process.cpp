#include "video_runtime_internal.h"

#include "application/runtime/app_ctrl.h"
#include "application/runtime/process_signals.h"
#include "application/runtime/service_watchdog.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <stdexcept>
#include <sys/resource.h>

volatile sig_atomic_t g_shutdown_signal = 0;

namespace vision
{
namespace runtime_detail
{
namespace
{
const int signals[] = {SIGINT, SIGTERM, SIGPIPE};
void stop_signal(int)
{
    g_shutdown_signal = 1;
    service_watchdog::request_shutdown();
}
} // namespace

SignalScope::SignalScope(bool enabled)
{
    if (!enabled)
        return;
    for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i)
    {
        struct sigaction action
        {
        };
        sigemptyset(&action.sa_mask);
        action.sa_handler = i < 2 ? stop_signal : SIG_IGN;
        if (sigaction(signals[i], &action, &previous_[i]) != 0)
        {
            const int error = errno;
            while (installed_ > 0)
            {
                --installed_;
                sigaction(signals[installed_], &previous_[installed_], nullptr);
            }
            throw std::runtime_error(std::string("Cannot install runtime signal handlers: ") + strerror(error));
        }
        ++installed_;
    }
}
SignalScope::~SignalScope()
{
    while (installed_ > 0)
    {
        --installed_;
        sigaction(signals[installed_], &previous_[installed_], nullptr);
    }
}

void raise_fd_limit_or_warn(void)
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
void *fd_monitor_thread(void *arg)
{
    (void)arg;
    while (g_pCtrl && g_pCtrl->isRunning.load() && !g_pCtrl->fd_monitor_exit.load())
    {
        /* 60 秒超时等待 */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 60;

        pthread_mutex_lock(&g_pCtrl->cv_config_mtx);
        int wait_result = 0;
        while (g_pCtrl->isRunning.load() && !g_pCtrl->fd_monitor_exit.load() && wait_result == 0)
            wait_result = pthread_cond_timedwait(&g_pCtrl->cv_config, &g_pCtrl->cv_config_mtx, &ts);
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

} // namespace runtime_detail
} // namespace vision
