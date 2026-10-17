#include "service_watchdog.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace service_watchdog
{
struct State
{
    const char *name;
    int channel;
    uint64_t timeout_ms;
    std::atomic<uint64_t> deadline{0};
};

namespace
{
std::mutex registry_mutex;
std::vector<std::weak_ptr<State>> registry;
std::atomic<uint64_t> main_deadline{0};
std::mutex wait_mutex;
std::condition_variable wakeup;
bool stopping = false;
std::thread worker;

uint64_t configured_timeout(const char *name, uint64_t fallback)
{
    const char *value = std::getenv(name);
    if (!value)
        return fallback;
    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    return end && !*end && parsed >= 30000 && parsed <= 1800000 ? parsed : fallback;
}
}

uint64_t startup_timeout_ms()
{
    static const uint64_t timeout = configured_timeout("RK_SERVICE_STARTUP_TIMEOUT_MS", 300000);
    return timeout;
}

static uint64_t task_timeout_ms()
{
    static const uint64_t timeout = configured_timeout("RK_SERVICE_TASK_TIMEOUT_MS", 120000);
    return timeout;
}

uint64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

Progress::Progress(const char *name, int channel, uint64_t timeout_ms) : state_(std::make_shared<State>())
{
    state_->name = name;
    state_->channel = channel;
    state_->timeout_ms = timeout_ms ? timeout_ms : task_timeout_ms();
    std::lock_guard<std::mutex> lock(registry_mutex);
    registry.emplace_back(state_);
}

void Progress::begin() { state_->deadline.store(now_ms() + state_->timeout_ms); }
void Progress::end() { state_->deadline.store(0); }
void main_heartbeat(uint64_t timeout_ms) { main_deadline.store(now_ms() + (timeout_ms ? timeout_ms : task_timeout_ms())); }

bool healthy_at(uint64_t current_ms, std::string &reason)
{
    const uint64_t main_limit = main_deadline.load();
    if (main_limit && current_ms > main_limit)
    {
        reason = "main loop/startup stalled";
        return false;
    }
    std::lock_guard<std::mutex> lock(registry_mutex);
    for (auto it = registry.begin(); it != registry.end();)
    {
        auto state = it->lock();
        if (!state)
        {
            it = registry.erase(it);
            continue;
        }
        const uint64_t deadline = state->deadline.load();
        if (deadline && current_ms > deadline)
        {
            reason = std::string(state->name) + " channel=" + std::to_string(state->channel) + " stalled";
            return false;
        }
        ++it;
    }
    return true;
}

bool notify(const std::string &message)
{
    const char *path = std::getenv("NOTIFY_SOCKET");
    if (!path || (path[0] != '/' && path[0] != '@'))
        return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const size_t length = std::strlen(path);
    if (length >= sizeof(address.sun_path))
        return false;
    std::memcpy(address.sun_path, path, length + 1);
    if (path[0] == '@')
        address.sun_path[0] = '\0';
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return false;
    const socklen_t size = offsetof(sockaddr_un, sun_path) + length + (path[0] == '@' ? 0 : 1);
    const bool sent = sendto(fd, message.data(), message.size(), MSG_NOSIGNAL,
                             reinterpret_cast<sockaddr *>(&address), size) == static_cast<ssize_t>(message.size());
    close(fd);
    return sent;
}

void start()
{
    const char *usec = std::getenv("WATCHDOG_USEC");
    const char *pid = std::getenv("WATCHDOG_PID");
    if (!usec || !std::getenv("NOTIFY_SOCKET") || (pid && std::strtol(pid, nullptr, 10) != getpid()))
        return;
    char *end = nullptr;
    const unsigned long long interval = std::strtoull(usec, &end, 10);
    if (!end || *end || interval < 1000 || worker.joinable())
        return;
    stopping = false;
    main_heartbeat(startup_timeout_ms());
    worker = std::thread([interval] {
        const auto period = std::chrono::milliseconds(std::min<unsigned long long>(5000, interval / 3000));
        bool failed = false;
        std::unique_lock<std::mutex> lock(wait_mutex);
        while (!stopping)
        {
            std::string reason;
            if (!failed && !healthy_at(now_ms(), reason))
            {
                failed = true; // Latch: a transient late completion must not hide a stalled worker.
                std::fprintf(stderr, "[Recovery] %s; withholding service heartbeat for restart\n", reason.c_str());
                notify("STATUS=" + reason);
            }
            if (!failed)
                notify("WATCHDOG=1");
            wakeup.wait_for(lock, period, [] { return stopping; });
        }
    });
}

void stop()
{
    {
        std::lock_guard<std::mutex> lock(wait_mutex);
        stopping = true;
    }
    wakeup.notify_all();
    if (worker.joinable())
        worker.join();
    main_deadline.store(0);
}
} // namespace service_watchdog
