#include "logic/core/logic_parameters.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main()
{
    LogicParameterValue object_a;
    object_a.type = LogicParameterType::OBJECT;
    object_a.text_value = "{\"a\":1,\"b\":2}";
    LogicParameterValue object_b = object_a;
    object_b.text_value = "{\"b\":2,\"a\":1}";
    assert(object_a == object_b);

    LogicParameterValue array_a;
    array_a.type = LogicParameterType::ARRAY;
    array_a.text_value = "[1,2]";
    LogicParameterValue array_b = array_a;
    array_b.text_value = "[2,1]";
    assert(array_a != array_b);

    LogicParameterSet defaults;
    std::string normalized;
    std::vector<LogicParameterError> errors;

    /* 空 logic 是合法的“无后处理”状态，只接受空参数对象。 */
    LogicParameterSet no_logic_parameters;
    assert(logic_parameters_resolve("", "{}", &normalized,
                                    &no_logic_parameters, &errors));
    assert(errors.empty());
    assert(normalized == "{}");
    assert(logic_parameters_reload_impact("", "{}", "{}") ==
           LogicReloadImpact::NONE);
    assert(!logic_parameters_resolve("", "{\"stale\":1}", nullptr,
                                     nullptr, &errors));
    assert(!errors.empty());

    assert(logic_parameters_resolve("logic_button_demo", "{}", &normalized,
                                    &defaults, &errors));
    assert(errors.empty());
    assert(defaults.has("pulse_duration_sec"));
    assert(std::fabs(defaults.get_float("pulse_duration_sec") - 3.0f) < 0.0001f);
    assert(normalized == "{\"pulse_duration_sec\":3}");

    LogicParameterSet configured;
    assert(logic_parameters_resolve(
        "logic_button_demo", "{\"pulse_duration_sec\":4.5}", nullptr,
        &configured, &errors));
    assert(std::fabs(configured.get_float("pulse_duration_sec") - 4.5f) < 0.0001f);

    assert(logic_parameters_reload_impact(
               "logic_button_demo", "{\"pulse_duration_sec\":3}",
               "{\"pulse_duration_sec\":4.5}") ==
           LogicReloadImpact::RESET_STATE);
    assert(logic_parameters_reload_impact(
               "logic_button_demo", "{}", "{\"pulse_duration_sec\":3}") ==
           LogicReloadImpact::NONE);

    LogicParameterSet periodic_defaults;
    assert(logic_parameters_resolve(
        "logic_periodic_snapshot_demo", "{}", &normalized,
        &periodic_defaults, &errors));
    assert(errors.empty());
    assert(periodic_defaults.get_int("report_interval_sec") == 10);
    assert(periodic_defaults.get_int("display_number") == 100);
    assert(normalized == "{\"report_interval_sec\":10,\"display_number\":100}");

    LogicParameterSet periodic_configured;
    assert(logic_parameters_resolve(
        "logic_periodic_snapshot_demo",
        "{\"report_interval_sec\":15,\"display_number\":2026}", nullptr,
        &periodic_configured, &errors));
    assert(periodic_configured.get_int("report_interval_sec") == 15);
    assert(periodic_configured.get_int("display_number") == 2026);

    assert(logic_parameters_reload_impact(
               "logic_periodic_snapshot_demo",
               "{\"report_interval_sec\":15,\"display_number\":100}",
               "{\"report_interval_sec\":15,\"display_number\":2026}") ==
           LogicReloadImpact::PRESERVE_STATE);
    assert(logic_parameters_reload_impact(
               "logic_periodic_snapshot_demo",
               "{\"report_interval_sec\":10,\"display_number\":2026}",
               "{\"report_interval_sec\":15,\"display_number\":2026}") ==
           LogicReloadImpact::RESET_STATE);

    assert(!logic_parameters_resolve(
        "logic_button_demo", "{\"pulse_duration_sec\":31}", nullptr,
        nullptr, &errors));
    assert(!errors.empty());

    assert(!logic_parameters_resolve(
        "logic_button_demo", "{\"typo\":1}", nullptr, nullptr, &errors));
    assert(!errors.empty());

    assert(!logic_parameters_resolve(
        "logic_button_demo",
        "{\"pulse_duration_sec\":3,\"pulse_duration_sec\":4}",
        nullptr, nullptr, &errors));
    assert(!errors.empty());

    assert(!logic_parameters_resolve(
        "logic_does_not_exist", "{}", nullptr, nullptr, &errors));
    assert(!errors.empty());

    std::puts("logic_parameters_unit_test: PASS");
    return 0;
}
