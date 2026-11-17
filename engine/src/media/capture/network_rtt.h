#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <sys/socket.h>
#include <thread>

// 每个实际视频源共享一个探测器。只接收已解析地址，不在后台线程执行 DNS。
class NetworkRttMonitor
{
  public:
    explicit NetworkRttMonitor(std::function<void(float)> publish);
    ~NetworkRttMonitor();
    void set_target(const sockaddr *address, socklen_t length);
    void clear_target();
    void stop();

  private:
    void run();
    std::function<void(float)> publish_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<bool> stopped_{false};
    sockaddr_storage target_{};
    socklen_t target_length_ = 0;
    unsigned generation_ = 0;
    std::thread thread_;
};
