#include <cassert>
#include <iostream>
#include <limits>
#include <parameters.h>
#include <stdexcept>

namespace
{
const char *catalog = R"JSON({
  "channel_logics": [{
    "name": "test_intrusion",
    "parameters": {
      "type": "object", "additionalProperties": false,
      "properties": {
        "hold_ms": {"type":"integer", "default":2000, "minimum":0, "maximum":60000, "x-hot-reload":"reset_state"},
        "gain": {"type":"number", "default":1},
        "score": {"type":"number", "default":0.9, "minimum":0, "maximum":1},
        "label": {"type":"string", "default":"person", "enum":["person","car"]},
        "enabled": {"type":"boolean", "default":true},
        "zones": {"type":"array", "default":["entrance"]},
        "options": {"type":"object", "default":{"audit":true}},
        "mode": {"type":"string", "default":"normal", "x-hot-reload":"restart_required"}
      }
    }
  }],
  "global_logics": []
})JSON";
} // namespace

int main()
{
    // Defaults must obey the same constraints as explicit configured values.
    // Rejected catalogs must not replace the last accepted catalog or seal resolution.
    for (const char *property :
         {R"({"type":"integer","default":10,"maximum":5})", R"({"type":"number","default":-1,"minimum":0})",
          R"({"type":"string","default":"dog","enum":["person","car"]})",
          R"({"type":"integer","default":1,"enum":[2,3]})", R"({"type":"boolean","default":true,"enum":[false]})",
          R"({"type":"array","default":[1],"enum":[[2]]})", R"({"type":"object","default":{"a":1},"enum":[{"a":2}]})",
          R"({"type":"integer","default":1,"enum":[]})", R"({"type":"integer","default":1,"enum":1})",
          R"({"type":"integer","default":1,"minimum":5,"maximum":0})", R"({"type":"number","default":1,"minimum":"0"})",
          R"({"type":"number","default":1,"maximum":1e999})", R"({"type":"string","default":"one","minimum":0})"})
    {
        const std::string invalid =
            std::string(R"({"channel_logics":[{"name":"bad_default","parameters":{"properties":{"value":)") + property +
            R"(}}}],"global_logics":[]})";
        bool rejected = false;
        try
        {
            vision::set_logic_catalog(invalid);
        }
        catch (const std::invalid_argument &)
        {
            rejected = true;
        }
        assert(rejected);
    }
    // Registration is transactional: malformed catalogs do not poison later valid startup.
    for (
        const auto &invalid : std::vector<std::string>{
            "not-json", "{}", std::string(catalog) + " garbage", std::string(catalog) + std::string("\0junk", 5),
            R"({"channel_logics":[{"name":"bad","parameters":{"properties":{"n":{"type":"number","default":1e300}}}}],"global_logics":[]})",
            R"({"channel_logics":[{"name":"bad","parameters":{"properties":{"n":{"type":"integer","default":"bad"}}}}],"global_logics":[]})"})
    {
        bool rejected = false;
        try
        {
            vision::set_logic_catalog(invalid);
        }
        catch (const std::invalid_argument &)
        {
            rejected = true;
        }
        assert(rejected);
    }
    vision::set_logic_catalog(catalog);
    LogicParameterSet parameters;
    std::string normalized;
    std::vector<LogicParameterError> errors;
    assert(logic_parameters_resolve("test_intrusion", "{}", &normalized, &parameters, &errors));
    assert(errors.empty());
    assert(parameters.get_int("hold_ms") == 2000);
    assert(parameters.get_float("score") > .89f && parameters.get_float("score") < .91f);
    assert(parameters.get_string("label") == "person" && parameters.get_bool("enabled"));
    assert(parameters.get_json("zones") == "[\"entrance\"]");
    assert(parameters.get_json("options") == "{\"audit\":true}");

    // A default and its explicit spelling resolve to identical typed values.
    LogicParameterSet explicit_defaults;
    assert(logic_parameters_resolve("test_intrusion", R"({"hold_ms":2000,"score":0.9,"label":"person"})", nullptr,
                                    &explicit_defaults, &errors));
    assert(*parameters.find("hold_ms") == *explicit_defaults.find("hold_ms"));
    assert(*parameters.find("score") == *explicit_defaults.find("score"));
    assert(*parameters.find("label") == *explicit_defaults.find("label"));

    assert(logic_parameters_resolve("test_intrusion", R"({"hold_ms":1000,"label":"car","enabled":false})", &normalized,
                                    &parameters, &errors));
    const std::string previous_json = normalized;
    for (const auto &invalid : std::vector<std::string>{
             "{} garbage", std::string("{}\0junk", 7), R"({"gain":1e300})", R"({"hold_ms":-1})", R"({"hold_ms":60001})",
             R"({"hold_ms":1.5})", R"({"score":1.1})", R"({"label":"dog"})", R"({"enabled":1})", R"({"unknown":1})",
             R"({"hold_ms":100,"hold_ms":200})", "[]"})
    {
        assert(!logic_parameters_resolve("test_intrusion", invalid, &normalized, &parameters, &errors));
        assert(!errors.empty());
        assert(normalized == previous_json);
        assert(parameters.get_int("hold_ms") == 1000 && parameters.get_string("label") == "car");
        assert(!parameters.get_bool("enabled"));
    }
    assert(!logic_parameters_resolve("missing", "{}", nullptr, nullptr, &errors));
    assert(logic_parameters_reload_impact("test_intrusion", "{}", "{}") == LogicReloadImpact::NONE);
    assert(logic_parameters_reload_impact("test_intrusion", "{}", R"({"score":0.95})") ==
           LogicReloadImpact::PRESERVE_STATE);
    assert(logic_parameters_reload_impact("test_intrusion", "{}", R"({"hold_ms":1000})") ==
           LogicReloadImpact::RESET_STATE);
    assert(logic_parameters_reload_impact("test_intrusion", "{}", R"({"mode":"audit"})") ==
           LogicReloadImpact::RESTART_REQUIRED);

    // Direct assignment bypasses the schema; the float accessor still rejects non-finite/overflow values.
    LogicParameterSet direct;
    for (double invalid :
         {1e300, -1e300, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
    {
        LogicParameterValue value;
        value.type = LogicParameterType::NUMBER;
        value.number_value = invalid;
        direct.assign("gain", value);
        assert(direct.get_float("gain") == 0.0f);
    }

    bool sealed = false;
    try
    {
        vision::set_logic_catalog(catalog);
    }
    catch (const std::logic_error &)
    {
        sealed = true;
    }
    assert(sealed);
    assert(logic_parameters_resolve("test_intrusion", "{}", nullptr, &parameters, &errors));
    assert(parameters.get_int("hold_ms") == 2000 && errors.empty());
    std::cout
        << "Public parameter catalog: registration, defaults, validation, atomic failure, reload and seal passed\n";
}
