#include "video_runtime_internal.h"

#include "application/control/file_playback_control.h"
#include "application/runtime/app_ctrl.h"
#include "application/runtime/pause_ctrl.h"
#include "application/runtime/process_signals.h"
#include "application/runtime/runtime_status.h"
#include "application/runtime/service_watchdog.h"
#include "application/runtime/shutdown_status.h"
#include "media/display/display.h"

#include <cstdio>
#include <exception>
#include <unistd.h>
#include <utility>

namespace vision
{
namespace
{
std::atomic_flag runtime_claimed = ATOMIC_FLAG_INIT;
struct RuntimeReservation
{
    explicit RuntimeReservation(std::atomic<bool> &active) : active_(active)
    {
        active_.store(true);
    }
    ~RuntimeReservation()
    {
        active_.store(false);
    }
    std::atomic<bool> &active_;
};
} // namespace

VideoRuntime::Impl::Impl(VideoRuntimeOptions runtime_options) : options(std::move(runtime_options))
{
}
VideoRuntime::Impl::~Impl() = default;
VideoRuntime::VideoRuntime(VideoRuntimeOptions options) : impl_(new Impl(std::move(options)))
{
}
VideoRuntime::~VideoRuntime() = default;

bool VideoRuntime::Impl::stopping() const
{
    return stop_requested.load() || g_shutdown_signal || (ctrl_initialized && !g_pCtrl->isRunning.load());
}
void VideoRuntime::Impl::request_stop()
{
    stop_requested.store(true);
    std::lock_guard<std::mutex> lock(control_mutex);
    if (ctrl_initialized)
        app_ctrl_request_stop();
}
void VideoRuntime::request_stop()
{
    impl_->request_stop();
}
bool VideoRuntime::is_running() const
{
    return impl_->active.load();
}

std::vector<VideoChannelStatus> VideoRuntime::channel_status() const
{
    std::lock_guard<std::mutex> lock(impl_->control_mutex);
    return impl_->ctrl_initialized ? app_ctrl_channel_status() : impl_->final_status;
}

std::string VideoRuntime::last_error() const
{
    std::lock_guard<std::mutex> lock(impl_->control_mutex);
    if (impl_->ctrl_initialized)
    {
        std::lock_guard<std::mutex> failure_lock(g_pCtrl->failure_mutex);
        if (g_pCtrl->worker_failed)
            return g_pCtrl->worker_error;
    }
    return impl_->error;
}

std::vector<FilePlaybackStatus> VideoRuntime::file_playback() const
{
    std::lock_guard<std::mutex> lock(impl_->control_mutex);
    return impl_->ctrl_initialized ? file_playback_status() : std::vector<FilePlaybackStatus>{};
}

bool VideoRuntime::set_file_paused(int channel_id, bool paused, std::string *error)
{
    std::lock_guard<std::mutex> lock(impl_->control_mutex);
    if (impl_->ctrl_initialized)
        return file_playback_set_paused(channel_id, paused, error);
    if (error)
        *error = "视频程序尚未启动或已经退出";
    return false;
}

bool VideoRuntime::seek_file(int channel_id, int64_t position_ms, std::string *error)
{
    std::lock_guard<std::mutex> lock(impl_->control_mutex);
    if (impl_->ctrl_initialized)
        return file_playback_seek(channel_id, position_ms, error);
    if (error)
        *error = "视频程序尚未启动或已经退出";
    return false;
}

int VideoRuntime::run()
{
    if (impl_->entered.exchange(true))
    {
        fprintf(stderr, "[Runtime] run() can only be called once per VideoRuntime\n");
        return 6;
    }
    if (impl_->stop_requested.load())
        return 0;
    if (runtime_claimed.test_and_set())
    {
        fprintf(stderr, "[Runtime] a VideoRuntime has already run in this process\n");
        return 6;
    }
    RuntimeReservation reservation(impl_->active);
    if (impl_->options.config_path.empty())
    {
        std::lock_guard<std::mutex> lock(impl_->control_mutex);
        impl_->error = "config_path must not be empty";
        fprintf(stderr, "[Runtime] config_path must not be empty\n");
        return -1;
    }
    g_shutdown_signal = 0;
    g_pause_toggle_signal = 0;
    try
    {
        shutdown_status_begin();
        runtime_detail::SignalScope signals(impl_->options.install_signal_handlers);
        service_watchdog::start();
        int exit_code = 0;
        try
        {
            exit_code = impl_->start();
            if (exit_code == 0 && !impl_->stopping())
                impl_->wait_for_stop();
        }
        catch (const std::exception &error)
        {
            std::lock_guard<std::mutex> lock(impl_->control_mutex);
            impl_->error = error.what();
            fprintf(stderr, "[Runtime] %s\n", error.what());
            exit_code = 1;
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(impl_->control_mutex);
            impl_->error = "unexpected startup/runtime exception";
            fprintf(stderr, "[Runtime] unexpected startup/runtime exception\n");
            exit_code = 1;
        }
        return impl_->shutdown(exit_code);
    }
    catch (const std::exception &error)
    {
        std::lock_guard<std::mutex> lock(impl_->control_mutex);
        impl_->error = error.what();
        fprintf(stderr, "[Runtime] %s\n", error.what());
        service_watchdog::stop();
        shutdown_status_complete(1, 0);
        return 1;
    }
}

void VideoRuntime::Impl::wait_for_stop()
{
    printf("\n[Main] === All threads started. Entering main loop. ===\n\n");
    service_watchdog::main_heartbeat();
    if (app_ctrl_get_enable_disp())
        display(&g_pCtrl->dispDesc);
    else
    {
        while (g_pCtrl->isRunning.load() && !stopping())
        {
            service_watchdog::main_heartbeat();
            if (g_pause_toggle_signal)
            {
                g_pause_toggle_signal = 0;
                pause_ctrl::toggle();
            }
            usleep(100 * 1000);
        }
    }
}
} // namespace vision
