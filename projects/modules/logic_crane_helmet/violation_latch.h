#pragma once

#include <cstdint>

namespace crane_safety
{

struct LatchUpdate
{
    bool active = false;
    bool triggered = false;
    bool cleared = false;
    uint64_t confirm_elapsed_ms = 0;
    uint64_t clear_elapsed_ms = 0;
};

/* 对抖动的逐帧布尔条件做“持续确认 + 延迟解除”。 */
class ViolationLatch
{
  public:
    LatchUpdate update(bool observed, uint64_t now_ms, uint64_t confirm_ms, uint64_t clear_ms)
    {
        LatchUpdate out;
        if (observed)
        {
            clear_since_ms_ = 0;
            if (!active_)
            {
                if (candidate_since_ms_ == 0)
                    candidate_since_ms_ = now_ms;
                if (confirm_ms == 0 || now_ms - candidate_since_ms_ >= confirm_ms)
                {
                    active_ = true;
                    candidate_since_ms_ = 0;
                    out.triggered = true;
                }
            }
        }
        else
        {
            candidate_since_ms_ = 0;
            if (active_)
            {
                if (clear_since_ms_ == 0)
                    clear_since_ms_ = now_ms;
                if (clear_ms == 0 || now_ms - clear_since_ms_ >= clear_ms)
                {
                    active_ = false;
                    clear_since_ms_ = 0;
                    out.cleared = true;
                }
            }
        }
        if (candidate_since_ms_ != 0 && now_ms >= candidate_since_ms_)
            out.confirm_elapsed_ms = now_ms - candidate_since_ms_;
        if (clear_since_ms_ != 0 && now_ms >= clear_since_ms_)
            out.clear_elapsed_ms = now_ms - clear_since_ms_;
        out.active = active_;
        return out;
    }

    void reset()
    {
        active_ = false;
        candidate_since_ms_ = 0;
        clear_since_ms_ = 0;
    }

    bool active() const
    {
        return active_;
    }

  private:
    bool active_ = false;
    uint64_t candidate_since_ms_ = 0;
    uint64_t clear_since_ms_ = 0;
};

} // namespace crane_safety
