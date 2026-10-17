#include "safety_controller.h"
#include <gpio.h>

#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace crane_safety;

namespace
{
std::map<std::string, int> levels;
std::vector<std::pair<std::string, int>> writes;
std::string fail_pin;
int fail_count = 0;
uint64_t fake_now_ms = 0;
uint64_t next_write_delay_ms = 0;

uint64_t fake_clock() { return fake_now_ms; }

class TestController : public SafetyController
{
  public:
    TestController() : SafetyController(fake_clock) {}
    HardwareStatus apply(const HardwareConfig &config, bool lamp_on, bool horn_on,
                         uint64_t now_ms, bool force_horn_off = false)
    {
        fake_now_ms = now_ms;
        return SafetyController::apply(config, lamp_on, horn_on, now_ms, force_horn_off);
    }
};

void reset_gpio()
{
    levels.clear();
    writes.clear();
    fail_pin.clear();
    fail_count = 0;
    fake_now_ms = 0;
    next_write_delay_ms = 0;
}

std::vector<int> horn_writes(const std::string &pin)
{
    std::vector<int> values;
    for (const auto &write : writes)
        if (write.first == pin) values.push_back(write.second);
    return values;
}

void short_alarm_and_retrigger(int active_level)
{
    reset_gpio();
    HardwareConfig config;
    config.horn_active_level = active_level;
    config.horn_min_alarm_ms = 5000;
    const int inactive = 1 - active_level;
    TestController controller;
    assert(!controller.apply(config, false, false, 0).horn_on);
    assert(levels[config.horn_pin] == inactive);
    assert(controller.apply(config, false, true, 1000).horn_on);
    assert(controller.apply(config, false, false, 1200).horn_on);
    // Another violation during the same continuous GPIO activation does not
    // interrupt the voice or restart the minimum playback timer.
    assert(controller.apply(config, false, true, 3000).horn_on);
    assert(controller.apply(config, false, false, 5999).horn_on);
    assert(!controller.apply(config, false, false, 6000).horn_on);
    assert((horn_writes(config.horn_pin) == std::vector<int>{inactive, active_level, inactive}));
    assert(controller.apply(config, false, true, 7000).horn_on);
    assert(controller.apply(config, false, false, 11999).horn_on);
    assert(!controller.apply(config, false, false, 12000).horn_on);
}

void sustained_alarm_and_zero_duration()
{
    reset_gpio();
    HardwareConfig config;
    config.horn_min_alarm_ms = 5000;
    TestController controller;
    assert(controller.apply(config, false, true, 1000).horn_on);
    assert(controller.apply(config, false, true, 6000).horn_on);
    assert(controller.apply(config, false, true, 12000).horn_on);
    assert(!controller.apply(config, false, false, 12001).horn_on);
    config.horn_min_alarm_ms = 0;
    assert(controller.apply(config, false, true, 13000).horn_on);
    assert(!controller.apply(config, false, false, 13000).horn_on);
}

void lamp_switch_and_second_apply_do_not_cut_playback()
{
    reset_gpio();
    HardwareConfig config;
    config.horn_min_alarm_ms = 5000;
    TestController controller;
    auto first = controller.apply(config, true, true, 1000);
    assert(first.lamp_changed && first.horn_on);
    // The global controller masks personnel alarms and applies again after a
    // relay change, using the same timestamp. The horn must remain active.
    assert(controller.apply(config, true, false, 1000).horn_on);
    assert(controller.apply(config, false, false, 2000).horn_on);
    config.lamp_pin = "GPIO6_A3";
    assert(controller.apply(config, true, false, 3000).horn_on);
    assert(levels["GPIO6_A2"] == 0);
    assert(!controller.apply(config, true, false, 6000).horn_on);
    assert((horn_writes(config.horn_pin) == std::vector<int>{1, 0}));
}

void manual_cancel_disable_and_shutdown()
{
    reset_gpio();
    HardwareConfig config;
    config.horn_min_alarm_ms = 5000;
    TestController controller;
    assert(controller.apply(config, true, true, 1000).horn_on);
    assert(!controller.apply(config, true, false, 1100, true).horn_on);
    assert(!controller.apply(config, true, false, 1200).horn_on);
    assert(levels[config.lamp_pin] == 1); // Silence leaves the lamp alone.
    assert(controller.apply(config, true, true, 2000).horn_on);
    assert(!controller.apply(config, false, true, 2100, true).horn_on);
    assert(levels[config.lamp_pin] == 0); // Emergency off overrides demand.
    assert(!controller.apply(config, false, false, 2200).horn_on);
    assert(controller.apply(config, true, true, 3000).horn_on);
    config.enabled = false;
    assert(!controller.apply(config, true, true, 3100).horn_on);
    assert(levels[config.lamp_pin] == 0);
    config.enabled = true;
    assert(!controller.apply(config, false, false, 3200).horn_on);
    assert(controller.apply(config, true, true, 4000).horn_on);
    controller.shutdown();
    assert(levels[config.horn_pin] == 0 && levels[config.lamp_pin] == 0);
}

void gpio_failures_start_timer_only_after_success_and_retry_off()
{
    reset_gpio();
    HardwareConfig config;
    config.horn_min_alarm_ms = 5000;
    TestController controller;
    controller.apply(config, false, false, 0);
    fail_pin = config.horn_pin; fail_count = 1;
    auto failed = controller.apply(config, false, true, 1000);
    assert(!failed.horn_ok && !failed.horn_on && levels[config.horn_pin] == 0);
    assert(controller.apply(config, false, true, 1500).horn_on);
    assert(controller.apply(config, false, false, 6000).horn_on);
    assert(controller.apply(config, false, false, 6499).horn_on);
    fail_count = 1;
    assert(!controller.apply(config, false, false, 6500).horn_ok);
    assert(levels[config.horn_pin] == 1);
    assert(!controller.apply(config, false, false, 6501).horn_on);
    assert(levels[config.horn_pin] == 0);
}

void changing_duration_applies_to_next_activation()
{
    reset_gpio();
    HardwareConfig config;
    config.horn_min_alarm_ms = 5000;
    TestController controller;
    assert(controller.apply(config, false, true, 1000).horn_on);
    config.horn_min_alarm_ms = 2000;
    assert(controller.apply(config, false, false, 3000).horn_on);
    assert(!controller.apply(config, false, false, 6000).horn_on);
    assert(controller.apply(config, false, true, 7000).horn_on);
    assert(controller.apply(config, false, false, 8999).horn_on);
    assert(!controller.apply(config, false, false, 9000).horn_on);
}

void changing_horn_pin_releases_old_output()
{
    reset_gpio();
    HardwareConfig config;
    config.horn_min_alarm_ms = 5000;
    TestController controller;
    assert(controller.apply(config, false, true, 1000).horn_on);
    config.horn_pin = "GPIO6_A1";
    assert(!controller.apply(config, false, false, 1100).horn_on);
    assert(levels["GPIO6_A0"] == 0 && levels["GPIO6_A1"] == 0);
    config.horn_active_level = 0;
    assert(controller.apply(config, false, true, 2000).horn_on);
    assert(controller.apply(config, false, false, 6999).horn_on);
    assert(!controller.apply(config, false, false, 7000).horn_on);
    assert(levels[config.horn_pin] == 1);
}

void gpio_latency_does_not_shorten_minimum_output_duration()
{
    reset_gpio();
    HardwareConfig config;
    config.horn_min_alarm_ms = 5000;
    TestController controller;
    controller.apply(config, false, false, 0);
    next_write_delay_ms = 250;
    assert(controller.apply(config, false, true, 1000).horn_on);
    assert(controller.apply(config, false, false, 6000).horn_on);
    assert(controller.apply(config, false, false, 6249).horn_on);
    assert(!controller.apply(config, false, false, 6250).horn_on);
    config.horn_min_alarm_ms = 0;
    next_write_delay_ms = 250;
    assert(controller.apply(config, false, true, 7000).horn_on);
    // Even within the original tick, zero means no extra hold despite GPIO
    // activation having completed after the tick's recorded timestamp.
    assert(!controller.apply(config, false, false, 7000).horn_on);
}
} // namespace

extern "C" int pin_out_val(const char *pin, int level)
{
    fake_now_ms += next_write_delay_ms;
    next_write_delay_ms = 0;
    if (fail_count > 0 && fail_pin == pin)
    {
        --fail_count;
        return -1;
    }
    levels[pin] = level;
    writes.emplace_back(pin, level);
    return 0;
}

int main()
{
    short_alarm_and_retrigger(1);
    short_alarm_and_retrigger(0);
    sustained_alarm_and_zero_duration();
    lamp_switch_and_second_apply_do_not_cut_playback();
    manual_cancel_disable_and_shutdown();
    gpio_failures_start_timer_only_after_success_and_retry_off();
    changing_duration_applies_to_next_activation();
    changing_horn_pin_releases_old_output();
    gpio_latency_does_not_shorten_minimum_output_duration();
    std::puts("Crane horn GPIO timing regression passed (9 scenarios)");
}
