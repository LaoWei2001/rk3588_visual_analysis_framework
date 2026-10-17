#pragma once

#include <cstdint>
#include <string>

namespace crane_safety
{

struct HardwareConfig
{
    bool enabled = true;
    std::string horn_pin = "GPIO6_A0";
    int horn_active_level = 1;
    uint64_t horn_min_alarm_ms = 0;
    std::string lamp_pin = "GPIO6_A2";
    int lamp_active_level = 1;
};

struct HardwareStatus
{
    bool horn_on = false;
    bool lamp_on = false;
    bool horn_ok = true;
    bool lamp_ok = true;
    bool lamp_changed = false;
};

class SafetyController
{
  public:
    SafetyController();
    explicit SafetyController(uint64_t (*clock)());
    ~SafetyController();

    SafetyController(const SafetyController &) = delete;
    SafetyController &operator=(const SafetyController &) = delete;

    // The minimum starts on a successful inactive-to-active GPIO write.
    // Manual cancellation bypasses it; repeated requests do not restart it.
    HardwareStatus apply(const HardwareConfig &config, bool lamp_on, bool horn_on,
                         uint64_t now_ms, bool force_horn_off = false);
    void shutdown();

  private:
    bool write_output(const std::string &pin, int level, int *last_level);
    void release_previous_pins_if_changed(const HardwareConfig &config);

    HardwareConfig config_;
    bool configured_ = false;
    int horn_level_ = -1;
    int lamp_level_ = -1;
    bool horn_hold_started_ = false;
    uint64_t horn_hold_since_ms_ = 0;
    uint64_t horn_hold_duration_ms_ = 0;
    uint64_t (*clock_)();
};

} // namespace crane_safety
