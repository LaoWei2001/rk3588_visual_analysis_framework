#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

inline uint64_t performance_now_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

/* 最近一秒实际完成的次数。查询也淘汰旧样本，停流时无需新帧即可归零。
 * 不做 EMA，不把启动前/暂停期间的空闲时间混入恢复后的采样窗口。 */
class FrameRateCounter
{
  public:
    void tick()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        append(performance_now_ms());
    }

    void tick(uint64_t now_ms)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        append(now_ms);
    }

    float value() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        prune(performance_now_ms());
        return static_cast<float>(ticks_.size());
    }

    float value(uint64_t now_ms) const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        prune(now_ms);
        return static_cast<float>(ticks_.size());
    }

    void reset()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        ticks_.clear();
    }

  private:
    void append(uint64_t now_ms)
    {
        prune(now_ms);
        ticks_.push_back(now_ms);
    }

    void prune(uint64_t now_ms) const
    {
        while (!ticks_.empty() && now_ms >= ticks_.front() && now_ms - ticks_.front() >= 1000)
            ticks_.pop_front();
    }

    mutable std::mutex mtx_;
    mutable std::deque<uint64_t> ticks_;
};

/* 每通道多个 worker 的累计值；吞吐和平均耗时使用同一实际窗口。
 * 平均耗时只包含成功任务，失败数量单独报告。 */
struct InferencePerfCounters
{
    std::mutex mtx;
    uint64_t wait_us{0}, lock_us{0}, pre_us{0}, npu_us{0};
    uint64_t post_us{0}, filter_nms_us{0}, total_us{0}, samples{0}, failures{0}, published{0};
    uint64_t last_log_ms{0};

    struct Snapshot
    {
        uint64_t samples, failures, published, elapsed_ms;
        uint64_t wait, lock, pre, npu, post, filter_nms, total;
    };

    void accumulate(uint64_t wait, uint64_t lock, uint64_t pre, uint64_t npu, uint64_t post, uint64_t filter_nms,
                    uint64_t total, bool wrote_new)
    {
        std::lock_guard<std::mutex> guard(mtx);
        wait_us += wait;
        lock_us += lock;
        pre_us += pre;
        npu_us += npu;
        post_us += post;
        filter_nms_us += filter_nms;
        total_us += total;
        ++samples;
        if (wrote_new)
            ++published;
    }

    void record_failure()
    {
        std::lock_guard<std::mutex> guard(mtx);
        ++failures;
    }

    void init(uint64_t now_ms)
    {
        std::lock_guard<std::mutex> guard(mtx);
        clear();
        last_log_ms = now_ms;
    }

    bool reset_if_due(uint64_t now_ms, uint64_t window_ms, Snapshot &out)
    {
        std::lock_guard<std::mutex> guard(mtx);
        if (now_ms < last_log_ms || now_ms - last_log_ms < window_ms || (samples == 0 && failures == 0))
            return false;
        out = Snapshot{samples, failures, published, now_ms - last_log_ms, wait_us, lock_us,
                       pre_us,  npu_us,   post_us,   filter_nms_us,        total_us};
        last_log_ms = now_ms;
        clear();
        return true;
    }

  private:
    void clear()
    {
        wait_us = lock_us = pre_us = npu_us = post_us = filter_nms_us = total_us = 0;
        samples = failures = published = 0;
    }
};
