#pragma once

#include <cmath>
#include <cstdint>

namespace crane_safety
{

static inline uint64_t seconds_to_ms(float seconds)
{
    if (!(seconds > 0.0f))
        return 0;
    return static_cast<uint64_t>(std::llround(static_cast<double>(seconds) * 1000.0));
}

} // namespace crane_safety
