#include "inference/npu_execution.h"
#include <cassert>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

int peer_infer(int core_mask);
namespace
{
std::atomic<int> active{0}, calls{0};
std::string failure;
}

extern "C" int rknn_inputs_set(rknn_context, uint32_t, rknn_input *)
{
    assert(active.load() == 0);
    return failure == "input" ? -22 : 0;
}

// Fake only the vendor API: the real execution/gate code runs across two
// translation units. The lease must cover get/release as well as submission.
extern "C" int rknn_run(rknn_context, rknn_run_extend *options)
{
    ++calls;
    assert(options && options->timeout_ms == 5000 && options->fence_fd == -1);
    if (failure == "run") return -22;
    assert(active.fetch_add(1) == 0);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    return 0;
}
extern "C" int rknn_outputs_get(rknn_context, uint32_t, rknn_output *, rknn_output_extend *)
{
    assert(active.load() == 1);
    std::this_thread::yield();
    return failure == "get" ? -110 : 0;
}
extern "C" int rknn_outputs_release(rknn_context, uint32_t, rknn_output *)
{
    assert(active.fetch_sub(1) == 1);
    return failure == "release" ? -22 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1)
    {
        failure = argv[1];
        assert(peer_infer(1) < 0);
        const int submitted = calls.load();
        // Failure in another model TU stops submissions, even on another core.
        for (int mask : {0, 1, 2, 4, 7})
        {
            bool rejected = false;
            try { npu::Execution next(mask); next.run(1); }
            catch (const std::runtime_error &) { rejected = true; }
            assert(rejected && calls == submitted);
        }
        return 0;
    }
    // Model decoder exceptions release the output before unlocking the core.
    try
    {
        npu::Execution execution(1);
        rknn_output output{};
        npu::Outputs outputs(execution, 1, 1, &output);
        assert(execution.run(1) == 0 && outputs.get() == 0);
        throw std::runtime_error("decoder failure");
    }
    catch (const std::runtime_error &) {}
    assert(active == 0);
    calls = 0;
    std::vector<std::thread> workers;
    for (int i = 0; i < 6; ++i)
        workers.emplace_back([i] {
            for (int frame = 0; frame < 80; ++frame)
            {
                if (i % 2) { assert(peer_infer(1) == 0); continue; }
                npu::Execution execution(1);
                rknn_output output{};
                assert(execution.run(1) == 0);
                assert(execution.outputs_get(1, 1, &output) == 0);
                assert(execution.outputs_release(1, 1, &output) == 0);
            }
        });
    for (auto &worker : workers) worker.join();
    assert(active == 0 && calls == 480);
}
