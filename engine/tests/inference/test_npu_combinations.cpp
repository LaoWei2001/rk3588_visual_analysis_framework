#include "inference/npu_core_gate.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace std::chrono;
static void wait_queued(unsigned count)
{
    const auto end = steady_clock::now() + seconds(2);
    while (npu::CoreLease::queued_count() < count && steady_clock::now() < end) std::this_thread::yield();
    assert(npu::CoreLease::queued_count() == count);
}
int main()
{
    // Every pair of masks (including AUTO/ALL), across seven model families.
    // Emulate occupied hardware until postprocessing/output release completes.
    const int masks[] = {0, 1, 2, 3, 4, 5, 6, 7, 0xffff};
    for (int a : masks) for (int b : masks)
    {
        std::future<void> other;
        {
            npu::CoreLease held(a);
            other = std::async(std::launch::async, [b] { npu::CoreLease lease(b); });
            const int left = (a == 0 || a == 0xffff) ? 7 : a;
            const int right = (b == 0 || b == 0xffff) ? 7 : b;
            if (left & right)
            {
                wait_queued(1);
                assert(other.wait_for(milliseconds(1)) == std::future_status::timeout);
            }
            else assert(other.wait_for(seconds(2)) == std::future_status::ready);
        }
        assert(other.wait_for(seconds(2)) == std::future_status::ready);
        other.get();
    }
    std::atomic<int> active{0}, completed{0};
    std::vector<std::thread> threads;
    for (int family = 0; family < 7; ++family)
        threads.emplace_back([&, family] {
            for (int round = 0; round < 20; ++round)
                for (int index = 0; index < 9; ++index)
                {
                    const int requested = masks[(index + family + round) % 9];
                    const int actual = requested == 0 || requested == 0xffff ? 7 : requested;
                    npu::CoreLease lease(requested);
                    assert((active.fetch_or(actual) & actual) == 0);
                    std::this_thread::sleep_for(microseconds(30));
                    assert((active.fetch_and(~actual) & actual) == actual);
                    ++completed;
                }
        });
    for (auto &thread : threads) thread.join();
    assert(active == 0 && completed == 7 * 20 * 9);

    // An older multi-core waiter must not be starved by newer single-core work.
    std::atomic<int> order{0};
    std::future<void> first, second;
    {
        npu::CoreLease occupied(2);
        first = std::async(std::launch::async, [&] { npu::CoreLease lease(3); assert(order++ == 0); });
        wait_queued(1);
        second = std::async(std::launch::async, [&] { npu::CoreLease lease(1); assert(order++ == 1); });
        wait_queued(2);
        // A disjoint core is still available while those requests wait.
        auto separate = std::async(std::launch::async, [] { npu::CoreLease lease(4); });
        assert(separate.wait_for(seconds(2)) == std::future_status::ready);
        separate.get();
    }
    first.get(); second.get();

    // Stop/fault wakes waiters BEFORE the currently running ioctl returns.
    for (bool fault : {false, true})
    {
        {
            npu::CoreLease occupied(7);
            auto waiter = std::async(std::launch::async, [] {
                try { npu::CoreLease lease(1); return false; }
                catch (const std::runtime_error &) { return true; }
            });
            wait_queued(1);
            if (fault) occupied.mark_failed(); else npu::CoreLease::stop();
            assert(waiter.wait_for(seconds(2)) == std::future_status::ready && waiter.get());
        }
        if (!fault) npu::CoreLease::resume();
    }
    bool rejected = false;
    try { npu::CoreLease::resume(); } catch (const std::runtime_error &) { rejected = true; }
    assert(rejected); // hot reload must never clear a driver failure
}
