#pragma once

#include <runtime.h>

#include <atomic>
#include <csignal>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <vector>

namespace vision
{
namespace runtime_detail
{
/** Process-wide changes belong to the run() scope, not to a library constructor. */
class SignalScope
{
  public:
    explicit SignalScope(bool enabled);
    ~SignalScope();
    SignalScope(const SignalScope &) = delete;
    SignalScope &operator=(const SignalScope &) = delete;

  private:
    struct sigaction previous_[4]{};
    unsigned installed_ = 0;
};
void raise_fd_limit_or_warn();
void *fd_monitor_thread(void *);
} // namespace runtime_detail

struct VideoRuntime::Impl
{
    explicit Impl(VideoRuntimeOptions runtime_options);
    ~Impl();
    int start();
    void start_capturers();
    int start_workers();
    void wait_for_stop();
    int shutdown(int exit_code);
    void request_stop();
    bool stopping() const;

    VideoRuntimeOptions options;
    std::atomic<bool> entered{false};
    std::atomic<bool> active{false};
    std::atomic<bool> stop_requested{false};
    // 保护控制块的生命周期和公开查询；逐帧路径不取得此锁。
    std::mutex control_mutex;
    bool ctrl_initialized = false;
    bool pipeline_initialized = false;
    bool config_monitor_started = false;
    bool fd_monitor_started = false;
    std::vector<VideoChannelStatus> final_status;
    std::string error;
    std::vector<pthread_t> display_tids;
    std::vector<pthread_t> dispatch_tids;
    std::vector<pthread_t> logic_tids;
};
} // namespace vision
