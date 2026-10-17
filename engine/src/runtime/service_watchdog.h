#pragma once

#include <cstdint>
#include <memory>
#include <string>

// Only executing work has a deadline. Waiting for a camera, a job or resume is idle.
namespace service_watchdog
{
struct State;
uint64_t now_ms();
uint64_t startup_timeout_ms();

class Progress
{
public:
    explicit Progress(const char *name, int channel = -1, uint64_t timeout_ms = 0);
    void begin();
    void end();
private:
    std::shared_ptr<State> state_;
};

class Scope
{
public:
    explicit Scope(Progress &progress) : progress_(progress) { progress_.begin(); }
    ~Scope() { progress_.end(); }
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
private:
    Progress &progress_;
};

// No systemd dependency; inactive for CLI runs without WATCHDOG_USEC/NOTIFY_SOCKET.
void start();
void stop();
void main_heartbeat(uint64_t timeout_ms = 0);
bool healthy_at(uint64_t current_ms, std::string &reason);
bool notify(const std::string &message);
} // namespace service_watchdog
