#include "safety_controller.h"

#include <gpio.h>
#include <chrono>

namespace crane_safety
{

namespace
{
uint64_t monotonic_ms()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

SafetyController::SafetyController() : clock_(monotonic_ms) {}
SafetyController::SafetyController(uint64_t (*clock)()) : clock_(clock ? clock : monotonic_ms) {}

SafetyController::~SafetyController()
{
    shutdown();
}

bool SafetyController::write_output(const std::string &pin, int level, int *last_level)
{
    if (!last_level || pin.empty())
        return false;
    level = level ? 1 : 0;
    if (*last_level == level)
        return true;
    if (pin_out_val(pin.c_str(), level) != 0)
        return false;
    *last_level = level;
    return true;
}

void SafetyController::release_previous_pins_if_changed(const HardwareConfig &config)
{
    if (!configured_)
        return;
    if (config_.horn_pin != config.horn_pin && !config_.horn_pin.empty())
        pin_out_val(config_.horn_pin.c_str(), 1 - (config_.horn_active_level ? 1 : 0));
    if (config_.lamp_pin != config.lamp_pin && !config_.lamp_pin.empty())
        pin_out_val(config_.lamp_pin.c_str(), 1 - (config_.lamp_active_level ? 1 : 0));
}

HardwareStatus SafetyController::apply(const HardwareConfig &config, bool lamp_on, bool horn_on,
                                      uint64_t now_ms, bool force_horn_off)
{
    HardwareStatus out;
    release_previous_pins_if_changed(config);
    if (!configured_ || config_.horn_pin != config.horn_pin ||
        config_.horn_active_level != config.horn_active_level || config_.enabled != config.enabled)
    {
        horn_level_ = -1;
        horn_hold_started_ = false;
    }
    if (!configured_ || config_.lamp_pin != config.lamp_pin ||
        config_.lamp_active_level != config.lamp_active_level || config_.enabled != config.enabled)
    {
        lamp_level_ = -1;
    }
    config_ = config;
    configured_ = true;

    if (!config.enabled)
    {
        horn_hold_started_ = false;
        const int previous_lamp_level = lamp_level_;
        out.horn_ok = write_output(config.horn_pin, 1 - (config.horn_active_level ? 1 : 0), &horn_level_);
        out.lamp_ok = write_output(config.lamp_pin, 1 - (config.lamp_active_level ? 1 : 0), &lamp_level_);
        out.lamp_changed = out.lamp_ok &&
                           (previous_lamp_level < 0 || previous_lamp_level != lamp_level_);
        return out;
    }

    const int previous_lamp_level = lamp_level_;
    if (force_horn_off)
    {
        horn_hold_started_ = false;
        horn_on = false;
    }
    const int active_horn_level = config.horn_active_level ? 1 : 0;
    const bool holding = horn_hold_started_ && horn_hold_duration_ms_ > 0 && horn_level_ == active_horn_level &&
                         (now_ms < horn_hold_since_ms_ || now_ms - horn_hold_since_ms_ < horn_hold_duration_ms_);
    const bool wanted_horn_on = horn_on || holding;
    const int previous_horn_level = horn_level_;
    const int wanted_horn = wanted_horn_on ? (config.horn_active_level ? 1 : 0)
                                    : 1 - (config.horn_active_level ? 1 : 0);
    const int wanted_lamp = lamp_on ? (config.lamp_active_level ? 1 : 0)
                                    : 1 - (config.lamp_active_level ? 1 : 0);
    out.horn_ok = write_output(config.horn_pin, wanted_horn, &horn_level_);
    if (out.horn_ok)
    {
        if (wanted_horn_on && previous_horn_level != active_horn_level)
        {
            horn_hold_started_ = true;
            // A GPIO write may take time. Start after success, using the same
            // monotonic clock as global ticks, so that latency cannot shorten
            // the minimum physical output duration.
            horn_hold_since_ms_ = clock_();
            // Snapshot the duration for this playback; configuration changes
            // apply on the next actual activation, without truncating this one.
            horn_hold_duration_ms_ = config.horn_min_alarm_ms;
        }
        else if (!wanted_horn_on)
            horn_hold_started_ = false;
    }
    out.lamp_ok = write_output(config.lamp_pin, wanted_lamp, &lamp_level_);
    out.horn_on = out.horn_ok && horn_level_ == (config.horn_active_level ? 1 : 0);
    out.lamp_on = out.lamp_ok && lamp_level_ == (config.lamp_active_level ? 1 : 0);
    out.lamp_changed = out.lamp_ok &&
                       (previous_lamp_level < 0 || previous_lamp_level != lamp_level_);
    return out;
}

void SafetyController::shutdown()
{
    if (!configured_)
        return;
    if (!config_.horn_pin.empty())
        pin_out_val(config_.horn_pin.c_str(), 1 - (config_.horn_active_level ? 1 : 0));
    if (!config_.lamp_pin.empty())
        pin_out_val(config_.lamp_pin.c_str(), 1 - (config_.lamp_active_level ? 1 : 0));
    horn_level_ = -1;
    lamp_level_ = -1;
    configured_ = false;
    horn_hold_started_ = false;
}

} // namespace crane_safety
