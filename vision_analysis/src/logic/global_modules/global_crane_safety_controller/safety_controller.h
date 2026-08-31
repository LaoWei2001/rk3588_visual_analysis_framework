#pragma once

#include <string>

namespace crane_safety
{

struct HardwareConfig
{
    bool enabled = true;
    std::string horn_pin = "GPIO6_A0";
    int horn_active_level = 1;
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
    SafetyController() = default;
    ~SafetyController();

    SafetyController(const SafetyController &) = delete;
    SafetyController &operator=(const SafetyController &) = delete;

    HardwareStatus apply(const HardwareConfig &config, bool lamp_on, bool horn_on);
    void shutdown();

  private:
    bool write_output(const std::string &pin, int level, int *last_level);
    void release_previous_pins_if_changed(const HardwareConfig &config);

    HardwareConfig config_;
    bool configured_ = false;
    int horn_level_ = -1;
    int lamp_level_ = -1;
};

} // namespace crane_safety
