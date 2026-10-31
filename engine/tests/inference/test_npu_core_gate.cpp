#include "inference/npu_core_gate.h"
#include "inference/npu_core_assignment.h"
#include <cassert>
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct Model
{
    bool enable = true;
    std::string model_path = "model.rknn", model_type = "yolov5";
    int npu_core = -1;
};
struct Channel
{
    int id = 0;
    bool enable = true, infer_enable = true;
    std::vector<Model> models{Model{}};
};

void global_core_planning()
{
    std::vector<Channel> channels(3);
    for (int i = 0; i < 3; ++i) channels[i].id = i;
    channels[1].models[0].npu_core = 1;
    channels[2].models[0].npu_core = 2;
    // The exact reported workload, then every other adapter in the AUTO slot.
    for (const char *type : {"zip_depth", "yolo26_depth", "yolov5", "yolov8_det",
                             "yolov8_pose", "yolo26_pose", "yolov5_seg"})
    {
        channels[0].models[0].model_type = type;
        assert(npu::explicitly_assigned_cores(channels) == 6);
        const int pool = npu::preferred_auto_core_pool(channels);
        assert(pool == 1);
        for (int channel = 0; channel < 15; ++channel)
            for (int worker = 0; worker < 9; ++worker)
                for (int model = 0; model < 7; ++model)
                    assert(npu::assigned_core_mask(-1, channel, worker, model, pool) == 1);
        assert(npu::assigned_core_mask(1, 0, 0, 0, pool) == 2);
        assert(npu::assigned_core_mask(2, 0, 0, 0, pool) == 4);
    }
    channels[1].models[0].npu_core = 0;
    assert(npu::preferred_auto_core_pool(channels) == 2); // Global hot-reload plan.
    channels[1].models[0].npu_core = 1;
    assert(npu::preferred_auto_core_pool(channels) == 1); // Failed proposal restored.
    channels[2].models[0].npu_core = 1;
    assert(npu::preferred_auto_core_pool(channels) == 5);
    int used = 0;
    for (int worker = 0; worker < 9; ++worker)
        used |= npu::assigned_core_mask(-1, 0, worker, 0, 5);
    assert(used == 5); // Spread over exactly the two unreserved cores.
    channels[0].models.push_back(Model{});
    channels[0].models.back().npu_core = 0; // Mixed composite also reserves its pin.
    assert(npu::preferred_auto_core_pool(channels) == 4);
    channels[2].models[0].npu_core = 2;
    assert(npu::preferred_auto_core_pool(channels) == 7); // All cores pinned: safe sharing.
    channels[0].models.back().enable = false;
    channels[1].infer_enable = false;
    channels[2].enable = false;
    assert(npu::preferred_auto_core_pool(channels) == 7);
    assert(npu::has_automatic_models(channels[0]));
    assert(!npu::has_automatic_models(channels[1]));
    channels[1].enable = channels[1].infer_enable = true;
    channels[1].models[0].model_path.clear();
    assert(npu::preferred_auto_core_pool(channels) == 7); // Invalid/inactive entry does not reserve.
    std::vector<Channel> all_auto(6);
    for (int i = 0; i < 6; ++i) all_auto[i].id = i * 2; // Sparse IDs must not collide when isolated.
    assert(npu::preferred_auto_core_pool(all_auto) == 7);
    for (int channel = 0; channel < 15; ++channel)
        for (int worker = 0; worker < 9; ++worker)
            for (int model = 0; model < 7; ++model)
                assert(npu::assigned_core_mask(-1, channel, worker, model, 7) ==
                       (1 << ((channel + worker + model) % 3)));
    for (const auto &ch : all_auto)
        assert(npu::channel_auto_placement(all_auto, ch.id).pool == 7); // Six-way sharing unchanged.
    all_auto.resize(3);
    for (int i = 0; i < 3; ++i)
    {
        const auto plan = npu::channel_auto_placement(all_auto, all_auto[i].id);
        assert(plan.pool == (1 << i));
        for (int w = 0; w < 3; ++w)
            assert(npu::assigned_core_mask(-1, all_auto[i].id, w, 0, plan.pool) == (1 << i));
    }
    all_auto.resize(2);
    for (int i = 0; i < 2; ++i)
    {
        const auto plan = npu::channel_auto_placement(all_auto, all_auto[i].id);
        assert(plan.pool == 7); // Two channels keep symmetric three-core queues.
        for (int w = 0; w < 3; ++w)
            assert(npu::assigned_core_mask(-1, all_auto[i].id, w, 0, plan.pool) ==
                   (1 << ((all_auto[i].id + w) % 3)));
    }
    all_auto.resize(1);
    for (int i = 0; i < 6; ++i) all_auto[0].models.push_back(Model{});
    assert(npu::channel_auto_placement(all_auto, all_auto[0].id).pool == 7); // One composite keeps all cores.

    // Two AUTO channels plus a pinned model have disjoint remaining cores.
    std::vector<Channel> mixed(3);
    mixed[0].id = 9;
    mixed[1].id = 2;
    mixed[2].id = 5;
    mixed[2].models[0].npu_core = 1;
    assert(npu::channel_auto_placement(mixed, 2).pool == 1);
    assert(npu::channel_auto_placement(mixed, 9).pool == 4);
    // Array order, disabled entries and sparse channel IDs do not change rank.
    std::swap(mixed[0], mixed[1]);
    assert(npu::channel_auto_placement(mixed, 2).pool == 1);
    assert(npu::channel_auto_placement(mixed, 9).pool == 4);
    mixed[1].infer_enable = false;
    assert(npu::channel_auto_placement(mixed, 2).pool == 5);
    mixed[1].infer_enable = true;
    mixed[2].models[0].npu_core = 0;
    assert(npu::channel_auto_placement(mixed, 2).pool == 2); // Changed reservation.
    mixed[2].models[0].npu_core = 1;
    assert(npu::channel_auto_placement(mixed, 2).pool == 1); // Reconciled rollback.
}
} // namespace

int main()
{
    global_core_planning();
    // Three workers use all cores; separate single-worker channels/children
    // are spread too. An explicit affinity always takes priority.
    int assigned = 0;
    for (int worker = 0; worker < 3; ++worker)
    {
        assigned |= npu::assigned_core_mask(-1, 0, worker, 0);
        assert(npu::assigned_core_mask(1, 2, worker, 1) == 2);
    }
    assert(assigned == 7);
    assert(npu::assigned_core_mask(-1, 0, 0, 0) != npu::assigned_core_mask(-1, 1, 0, 0));
    assert(npu::assigned_core_mask(-1, 0, 0, 0) != npu::assigned_core_mask(-1, 0, 0, 1));

    // Independent contexts sharing one explicitly pinned core never overlap.
    std::atomic<int> active{0}, completed{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 3; ++i)
        workers.emplace_back([&] {
            for (int frame = 0; frame < 80; ++frame)
            {
                npu::CoreLease lease(1);
                assert(active.fetch_add(1) == 0);
                std::this_thread::yield();
                assert(active.fetch_sub(1) == 1);
                ++completed;
            }
        });
    for (auto &worker : workers) worker.join();
    assert(completed == 240);

    using namespace std::chrono;
    // Different cores remain parallel; AUTO must wait for every occupied core.
    std::future<void> automatic;
    {
        npu::CoreLease first(1);
        auto separate = std::async(std::launch::async, [] { npu::CoreLease other(2); });
        assert(separate.wait_for(seconds(2)) == std::future_status::ready);
        separate.get();
        std::promise<void> started;
        auto ready = started.get_future();
        automatic = std::async(std::launch::async, [&started] {
            started.set_value();
            npu::CoreLease all(0);
        });
        ready.wait();
        assert(automatic.wait_for(milliseconds(30)) == std::future_status::timeout);
    }
    assert(automatic.wait_for(seconds(2)) == std::future_status::ready);
    automatic.get();

    // An exception releases every held lock. A failed device is not resubmitted from any core.
    try { npu::CoreLease lease(3); throw 1; } catch (int) {}
    { npu::CoreLease lease(1); lease.mark_failed(); }
    for (const int mask : {1, 2, 3, 4, 7, 0})
    {
        bool rejected = false;
        try { npu::CoreLease lease(mask); } catch (const std::runtime_error &) { rejected = true; }
        assert(rejected);
    }
}
