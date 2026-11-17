// Exercises public lifecycle guards without opening a camera, NPU lock, or GPIO.
#include <runtime.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <thread>

static bool require(bool condition, const char *message)
{
    if (!condition)
        std::fprintf(stderr, "video runtime lifecycle: %s\n", message);
    return condition;
}

int main()
{
    vision::VideoRuntimeOptions options;
    vision::VideoRuntime cancelled(options);
    cancelled.request_stop();
    if (!require(!cancelled.is_running(), "new runtime is idle") ||
        !require(cancelled.run() == 0, "cancel before start must not initialize resources") ||
        !require(cancelled.run() == 6, "same instance cannot run twice"))
        return 1;

    // Empty configuration fails before any device/service startup. Two instances
    // race for the process reservation; exactly one reaches config validation.
    vision::VideoRuntime first(options), second(options);
    std::atomic<unsigned> ready{0};
    std::atomic<bool> go{false};
    int first_result = 0, second_result = 0;
    auto run = [&](vision::VideoRuntime &runtime, int &result) {
        ready.fetch_add(1);
        while (!go.load())
            std::this_thread::yield();
        result = runtime.run();
    };
    std::thread a(run, std::ref(first), std::ref(first_result));
    std::thread b(run, std::ref(second), std::ref(second_result));
    while (ready.load() != 2)
        std::this_thread::yield();
    go.store(true);
    a.join();
    b.join();
    if (!require((first_result == -1 && second_result == 6) || (first_result == 6 && second_result == -1),
                 "only one instance may attempt process initialization") ||
        !require(!first.is_running() && !second.is_running(), "failed startup leaves both runtimes idle"))
        return 1;
    first.request_stop();
    second.request_stop();
    vision::VideoRuntime restart(options);
    if (!require(restart.run() == 6, "new instance must not reinitialize global services"))
        return 1;
    std::puts("video runtime lifecycle guards passed");
    return 0;
}
