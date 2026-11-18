#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <utility>

// 一个文件帧从入队到业务处理结束共用一张凭据；覆盖、失败、退出也会自动归还。
// 计数对象独立存活，队列清理不依赖通道控制块的析构顺序。
struct FrameActivity
{
    explicit FrameActivity(std::shared_ptr<std::atomic<unsigned>> count, std::function<void()> on_idle = {})
        : pending(std::move(count)), idle(std::move(on_idle))
    {
        pending->fetch_add(1);
    }
    ~FrameActivity()
    {
        if (pending->fetch_sub(1) == 1 && idle)
            idle();
    }
    FrameActivity(const FrameActivity &) = delete;
    FrameActivity &operator=(const FrameActivity &) = delete;
    std::shared_ptr<std::atomic<unsigned>> pending;
    std::function<void()> idle;
};
