#include "common/performance_metrics.h"

#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

void rates_follow_completions_without_startup_or_resume_dilution()
{
    FrameRateCounter rate;
    assert(rate.value(10000) == 0);
    for (uint64_t now = 10040; now <= 11000; now += 40)
        rate.tick(now);
    assert(rate.value(11000) == 25);
    assert(rate.value(11400) == 15); // 旧样本在查询时淘汰，无需等下一帧。
    assert(rate.value(12000) == 0);
    for (uint64_t now = 30040; now <= 31000; now += 40)
        rate.tick(now);
    assert(rate.value(31000) == 25); // 19 秒空闲不稀释恢复后的窗口。
    rate.reset();
    assert(rate.value(31000) == 0);
    rate.tick(40000);
    assert(rate.value(40999) == 1);
    assert(rate.value(41000) == 0); // 边界样本只属于一个窗口。
}

void rates_aggregate_workers_and_track_a_drop_without_smoothing()
{
    FrameRateCounter rate;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker)
        workers.emplace_back([&] {
            for (int i = 0; i < 25; ++i)
                rate.tick(1000);
        });
    for (auto &worker : workers)
        worker.join();
    assert(rate.value(1000) == 100);
    for (uint64_t now = 2100; now <= 3000; now += 100)
        rate.tick(now);
    assert(rate.value(3000) == 10);
}

void performance_reports_actual_windows_and_separates_failed_or_superseded_tasks()
{
    InferencePerfCounters counters;
    counters.init(1000);
    counters.accumulate(1000, 2000, 3000, 4000, 5000, 6000, 20000, true);
    counters.accumulate(1000, 2000, 3000, 4000, 5000, 6000, 20000, false);
    counters.record_failure();
    InferencePerfCounters::Snapshot snapshot{};
    assert(!counters.reset_if_due(5999, 5000, snapshot));
    assert(counters.reset_if_due(21000, 5000, snapshot));
    assert(snapshot.elapsed_ms == 20000);
    assert(snapshot.samples == 2 && snapshot.published == 1 && snapshot.failures == 1);
    assert(snapshot.total == 40000 && snapshot.wait == 2000);
    assert(!counters.reset_if_due(26000, 5000, snapshot));
    counters.record_failure();
    assert(counters.reset_if_due(27000, 5000, snapshot));
    assert(snapshot.elapsed_ms == 6000 && snapshot.failures == 1 && snapshot.samples == 0);
    assert(snapshot.total == 0 && snapshot.published == 0);
}

void performance_aggregates_concurrent_workers()
{
    InferencePerfCounters counters;
    counters.init(0);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker)
        workers.emplace_back([&] {
            for (int i = 0; i < 100; ++i)
                counters.accumulate(1, 2, 3, 4, 5, 6, 20, i % 2 == 0);
        });
    for (auto &worker : workers)
        worker.join();
    InferencePerfCounters::Snapshot snapshot{};
    assert(counters.reset_if_due(5000, 5000, snapshot));
    assert(snapshot.elapsed_ms == 5000 && snapshot.samples == 400 && snapshot.published == 200);
    assert(snapshot.total == 8000 && snapshot.pre == 1200);
}

int main()
{
    rates_follow_completions_without_startup_or_resume_dilution();
    rates_aggregate_workers_and_track_a_drop_without_smoothing();
    performance_reports_actual_windows_and_separates_failed_or_superseded_tasks();
    performance_aggregates_concurrent_workers();
    std::cout << "Performance window, idle recovery and concurrent counter regressions passed\n";
}
