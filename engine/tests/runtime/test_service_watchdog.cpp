#include "runtime/service_watchdog.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

int main(int argc, char **argv)
{
    using namespace service_watchdog;
    if (argc == 2 && std::strcmp(argv[1], "--watchdog-protocol") == 0)
    {
        std::puts("rk3588-service-watchdog-v1");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--service-stall") == 0)
    {
        start();
        Progress progress("integration stalled inference", 3, 400);
        Scope scope(progress);
        while (true)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::string reason;
    {
        Progress progress("inference", 7, 120000);
        // Idle camera/paused/EOF worker has no deadline, even after a long time.
        assert(healthy_at(now_ms() + 1000000, reason));
        {
            Scope scope(progress);
            assert(healthy_at(now_ms(), reason));
            assert(!healthy_at(now_ms() + 120001, reason));
            assert(reason.find("inference channel=7") != std::string::npos);
        }
        assert(healthy_at(now_ms() + 1000000, reason));
        progress.begin();
    }
    // Destroyed/reloaded workers leave no permanent failed deadline.
    assert(healthy_at(now_ms() + 1000000, reason));
    main_heartbeat(120000);
    assert(healthy_at(now_ms(), reason));
    assert(!healthy_at(now_ms() + 120001, reason));
    stop();
    assert(healthy_at(now_ms() + 1000000, reason));
    std::puts("service watchdog regression passed");
}
