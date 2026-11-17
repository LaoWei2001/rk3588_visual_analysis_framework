#include "media/capture/network_rtt.h"
#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <network.h>

// 仅此测试丢弃发出的探测包，验证超时和退出；不修改网络配置。
static std::atomic<bool> drop_requests{false};
static std::atomic<int> sent_requests{0};
extern "C" ssize_t __real_send(int, const void *, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void *data, size_t length, int flags)
{
    ++sent_requests;
    if (drop_requests)
        return static_cast<ssize_t>(length);
    return __real_send(fd, data, length, flags);
}

int main()
{
    using Clock = std::chrono::steady_clock;
    assert(vision::measure_network_rtt("rtsp://127.0.0.1/live").milliseconds < 0);
    assert(vision::measure_network_rtt("localhost").milliseconds < 0);
    assert(vision::measure_network_rtt("127.0.0.1", 0).milliseconds < 0);
    const auto loopback = vision::measure_network_rtt("127.0.0.1");
    if (loopback.milliseconds < 0 && loopback.error.find("Cannot open ICMP socket") != std::string::npos)
    {
        std::cout << "ICMP permission unavailable: " << loopback.error << '\n';
        return 77;
    }
    assert(loopback.milliseconds >= 0 && loopback.error.empty());
    const auto ipv6 = vision::measure_network_rtt("::1");
    assert(ipv6.milliseconds >= 0 && ipv6.error.empty());

    drop_requests = true;
    auto before = Clock::now();
    const auto timeout = vision::measure_network_rtt("127.0.0.1", 80);
    assert(timeout.milliseconds < 0 && !timeout.error.empty());
    assert(Clock::now() - before < std::chrono::milliseconds(500));
    drop_requests = false;

    std::mutex mutex;
    std::condition_variable changed;
    float measured = -1;
    int publications = 0;
    NetworkRttMonitor monitor([&](float value) {
        std::lock_guard<std::mutex> lock(mutex);
        measured = value;
        ++publications;
        changed.notify_one();
    });
    sockaddr_in address{};
    address.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    monitor.set_target(reinterpret_cast<sockaddr *>(&address), sizeof(address));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(changed.wait_for(lock, std::chrono::seconds(2), [&] { return measured >= 0; }));
    }
    monitor.clear_target();
    {
        std::lock_guard<std::mutex> lock(mutex);
        assert(measured == -1);
    }
    drop_requests = true;
    const int sent_before = sent_requests;
    monitor.set_target(reinterpret_cast<sockaddr *>(&address), sizeof(address));
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    while (sent_requests == sent_before && Clock::now() < deadline)
        std::this_thread::yield();
    assert(sent_requests > sent_before);
    before = Clock::now();
    monitor.stop();
    assert(Clock::now() - before < std::chrono::milliseconds(300));
    assert(measured == -1);
    const int after_stop = publications;
    monitor.set_target(reinterpret_cast<sockaddr *>(&address), sizeof(address));
    monitor.stop();
    assert(publications == after_stop);
    std::cout << "IPv4/IPv6 RTT, timeout, target reset and cancellation passed\n";
}
