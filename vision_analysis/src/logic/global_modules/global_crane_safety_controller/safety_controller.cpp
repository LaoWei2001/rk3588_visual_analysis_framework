#include "logic/global_modules/global_crane_safety_controller/safety_controller.h"

#include "gpio/gpio.h"

namespace crane_safety
{

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

HardwareStatus SafetyController::apply(const HardwareConfig &config, bool lamp_on, bool horn_on)
{
    HardwareStatus out;
    release_previous_pins_if_changed(config);
    const bool config_changed = !configured_ || config_.horn_pin != config.horn_pin ||
                                config_.lamp_pin != config.lamp_pin ||
                                config_.horn_active_level != config.horn_active_level ||
                                config_.lamp_active_level != config.lamp_active_level ||
                                config_.enabled != config.enabled;
    if (config_changed)
    {
        config_ = config;
        configured_ = true;
        horn_level_ = -1;
        lamp_level_ = -1;
    }

    if (!config.enabled)
    {
        const int previous_lamp_level = lamp_level_;
        out.horn_ok = write_output(config.horn_pin, 1 - (config.horn_active_level ? 1 : 0), &horn_level_);
        out.lamp_ok = write_output(config.lamp_pin, 1 - (config.lamp_active_level ? 1 : 0), &lamp_level_);
        out.lamp_changed = out.lamp_ok &&
                           (previous_lamp_level < 0 || previous_lamp_level != lamp_level_);
        return out;
    }

    const int previous_lamp_level = lamp_level_;
    const int wanted_horn = horn_on ? (config.horn_active_level ? 1 : 0)
                                    : 1 - (config.horn_active_level ? 1 : 0);
    const int wanted_lamp = lamp_on ? (config.lamp_active_level ? 1 : 0)
                                    : 1 - (config.lamp_active_level ? 1 : 0);
    out.horn_ok = write_output(config.horn_pin, wanted_horn, &horn_level_);
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
}

} // namespace crane_safety
