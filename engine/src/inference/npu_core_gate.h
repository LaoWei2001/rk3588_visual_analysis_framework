#pragma once

#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace npu
{
class Cancelled : public std::runtime_error
{
  public:
    Cancelled() : std::runtime_error("NPU inference is stopping") {}
};

// Reserve an entire mask atomically. Older overlapping waiters have priority;
// independent masks may pass them. No per-frame heap allocation or partial locks.
class CoreLease
{
  public:
    explicit CoreLease(int core_mask) : mask_(normalize(core_mask))
    {
        auto &s = state();
        std::unique_lock<std::mutex> lock(s.mutex);
        check(s);
        Waiter waiter{mask_, nullptr};
        if (s.tail) s.tail->next = &waiter;
        else s.head = &waiter;
        s.tail = &waiter;
        try
        {
            s.changed.wait(lock, [&] { return s.failed || s.stopping || available(s, waiter); });
            check(s);
        }
        catch (...)
        {
            remove(s, waiter);
            s.changed.notify_all();
            throw;
        }
        remove(s, waiter);
        s.active |= mask_;
    }
    ~CoreLease()
    {
        auto &s = state();
        { std::lock_guard<std::mutex> lock(s.mutex); s.active &= ~mask_; }
        s.changed.notify_all();
    }
    CoreLease(const CoreLease &) = delete;
    CoreLease &operator=(const CoreLease &) = delete;
    void check_health() const
    {
        auto &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        check(s);
    }
    void mark_failed()
    {
        auto &s = state();
        { std::lock_guard<std::mutex> lock(s.mutex); s.failed = true; }
        s.changed.notify_all();
    }
    static void stop()
    {
        auto &s = state();
        { std::lock_guard<std::mutex> lock(s.mutex); s.stopping = true; }
        s.changed.notify_all();
    }
    static void resume()
    {
        auto &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.active || s.head) throw std::logic_error("NPU still has active or queued work");
        if (s.failed) throw std::runtime_error("Recover device and restart inference after NPU failure");
        s.stopping = false;
    }
    static unsigned queued_count()
    {
        auto &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        unsigned count = 0;
        for (auto *w = s.head; w; w = w->next) ++count;
        return count;
    }

  private:
    struct Waiter { int mask; Waiter *next; };
    struct State
    {
        std::mutex mutex;
        std::condition_variable changed;
        int active = 0;
        bool failed = false, stopping = false;
        Waiter *head = nullptr, *tail = nullptr;
    };
    static State &state() { static State value; return value; }
    static int normalize(int mask)
    {
        if (mask == 0 || mask == 0xffff) return 7;
        if (mask < 1 || mask > 7) throw std::invalid_argument("Invalid NPU core mask");
        return mask;
    }
    static void check(const State &s)
    {
        if (s.failed)
            throw std::runtime_error("NPU submission disabled after runtime failure; recover device and restart inference");
        if (s.stopping) throw Cancelled();
    }
    static bool available(const State &s, const Waiter &waiter)
    {
        if (s.active & waiter.mask) return false;
        for (auto *w = s.head; w != &waiter; w = w->next)
            if (w->mask & waiter.mask) return false;
        return true;
    }
    static void remove(State &s, Waiter &waiter)
    {
        Waiter *previous = nullptr;
        for (auto *w = s.head; w; previous = w, w = w->next)
            if (w == &waiter)
            {
                if (previous) previous->next = w->next;
                else s.head = w->next;
                if (s.tail == w) s.tail = previous;
                return;
            }
    }
    int mask_;
};
} // namespace npu
