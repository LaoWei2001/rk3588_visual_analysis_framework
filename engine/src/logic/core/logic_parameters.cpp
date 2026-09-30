/**
 * @file logic_parameters.cpp
 * @brief 嵌入式模块 Schema 的解析、配置规范化与热重载差异计算。
 */
#include "logic_parameters.h"

#include "third_party/json/cJSON.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <utility>

/* 由 build/generated/logic_catalog_embedded.cpp 提供。 */
extern const char *logic_embedded_catalog_json();

namespace
{
constexpr double kMaxExactJsonInteger = 9007199254740991.0; /* 2^53 - 1 */

struct ParameterSpec
{
    std::string key;
    LogicParameterType type = LogicParameterType::STRING;
    LogicParameterValue default_value;
    LogicReloadImpact reload_impact = LogicReloadImpact::PRESERVE_STATE;
    bool has_minimum = false;
    bool has_maximum = false;
    double minimum = 0.0;
    double maximum = 0.0;
    std::vector<LogicParameterValue> enum_values;
};

struct LogicSchema
{
    std::vector<ParameterSpec> ordered_specs;
    std::unordered_map<std::string, size_t> spec_index;
    bool additional_properties = false;
};

class LogicSchemaRegistry
{
  public:
    LogicSchemaRegistry()
    {
        load();
    }

    const LogicSchema *find(const std::string &logic_name) const
    {
        /* 空名称是框架原生的“无后处理”状态，不属于任何 modules/ 业务模块。
         * 隐式空 Schema 让配置仍走统一校验：只允许 {}，不接受悬空模块参数。 */
        if (logic_name.empty())
        {
            static const LogicSchema no_logic_schema;
            return &no_logic_schema;
        }
        const auto it = schemas_.find(logic_name);
        return it == schemas_.end() ? nullptr : &it->second;
    }

    bool valid() const
    {
        return error_.empty();
    }
    const std::string &error() const
    {
        return error_;
    }

  private:
    static bool parse_type(const char *name, LogicParameterType &out)
    {
        if (!name)
            return false;
        const std::string type(name);
        if (type == "string")
            out = LogicParameterType::STRING;
        else if (type == "number")
            out = LogicParameterType::NUMBER;
        else if (type == "integer")
            out = LogicParameterType::INTEGER;
        else if (type == "boolean")
            out = LogicParameterType::BOOLEAN;
        else if (type == "array")
            out = LogicParameterType::ARRAY;
        else if (type == "object")
            out = LogicParameterType::OBJECT;
        else
            return false;
        return true;
    }

    static bool parse_value(cJSON *item, LogicParameterType type, LogicParameterValue &out, std::string &error)
    {
        out = LogicParameterValue();
        out.type = type;
        switch (type)
        {
        case LogicParameterType::STRING:
            if (!cJSON_IsString(item) || !item->valuestring)
            {
                error = "expected string";
                return false;
            }
            out.text_value = item->valuestring;
            return true;
        case LogicParameterType::NUMBER:
            if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble))
            {
                error = "expected finite number";
                return false;
            }
            out.number_value = item->valuedouble;
            return true;
        case LogicParameterType::INTEGER: {
            if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
                std::floor(item->valuedouble) != item->valuedouble || item->valuedouble < -kMaxExactJsonInteger ||
                item->valuedouble > kMaxExactJsonInteger)
            {
                error = "expected integer";
                return false;
            }
            out.integer_value = static_cast<int64_t>(item->valuedouble);
            return true;
        }
        case LogicParameterType::BOOLEAN:
            if (!cJSON_IsBool(item))
            {
                error = "expected boolean";
                return false;
            }
            out.bool_value = cJSON_IsTrue(item);
            return true;
        case LogicParameterType::ARRAY:
        case LogicParameterType::OBJECT: {
            const bool expected = type == LogicParameterType::ARRAY ? cJSON_IsArray(item) : cJSON_IsObject(item);
            if (!expected)
            {
                error = type == LogicParameterType::ARRAY ? "expected array" : "expected object";
                return false;
            }
            char *text = cJSON_PrintUnformatted(item);
            if (!text)
            {
                error = "cannot serialize JSON value";
                return false;
            }
            out.text_value = text;
            cJSON_free(text);
            return true;
        }
        }
        error = "unsupported parameter type";
        return false;
    }

    static LogicReloadImpact parse_reload_impact(cJSON *property)
    {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(property, "x-hot-reload");
        if (!cJSON_IsString(item) || !item->valuestring)
            return LogicReloadImpact::PRESERVE_STATE;
        const std::string policy(item->valuestring);
        if (policy == "reset_state")
            return LogicReloadImpact::RESET_STATE;
        if (policy == "restart_required")
            return LogicReloadImpact::RESTART_REQUIRED;
        return LogicReloadImpact::PRESERVE_STATE;
    }

    void fail(const std::string &message)
    {
        if (error_.empty())
            error_ = message;
    }

    void load()
    {
        const char *catalog_text = logic_embedded_catalog_json();
        cJSON *root = catalog_text ? cJSON_Parse(catalog_text) : nullptr;
        if (!root)
        {
            fail("embedded logic catalog is invalid JSON");
            return;
        }

        const char *collections[] = {"channel_logics", "global_logics"};
        for (const char *collection : collections)
        {
            cJSON *logics = cJSON_GetObjectItemCaseSensitive(root, collection);
            if (!cJSON_IsArray(logics))
            {
                fail(std::string("embedded logic catalog has no ") + collection + " array");
                break;
            }

            cJSON *logic = nullptr;
            cJSON_ArrayForEach(logic, logics)
            {
                cJSON *name_item = cJSON_GetObjectItemCaseSensitive(logic, "name");
                cJSON *schema_item = cJSON_GetObjectItemCaseSensitive(logic, "parameters");
                if (!cJSON_IsString(name_item) || !name_item->valuestring || !cJSON_IsObject(schema_item))
                {
                    fail("logic catalog entry is missing name/parameters");
                    break;
                }

                const std::string logic_name(name_item->valuestring);
                LogicSchema schema;
                cJSON *additional = cJSON_GetObjectItemCaseSensitive(schema_item, "additionalProperties");
                schema.additional_properties = cJSON_IsTrue(additional);
                cJSON *properties = cJSON_GetObjectItemCaseSensitive(schema_item, "properties");
                if (!cJSON_IsObject(properties))
                {
                    fail(logic_name + ": parameters.properties is missing");
                    break;
                }

                for (cJSON *property = properties->child; property; property = property->next)
                {
                    if (!property->string || !cJSON_IsObject(property))
                    {
                        fail(logic_name + ": invalid parameter property");
                        break;
                    }
                    ParameterSpec spec;
                    spec.key = property->string;
                    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(property, "type");
                    if (!cJSON_IsString(type_item) || !parse_type(type_item->valuestring, spec.type))
                    {
                        fail(logic_name + "." + spec.key + ": invalid type");
                        break;
                    }
                    cJSON *default_item = cJSON_GetObjectItemCaseSensitive(property, "default");
                    std::string value_error;
                    if (!default_item || !parse_value(default_item, spec.type, spec.default_value, value_error))
                    {
                        fail(logic_name + "." + spec.key + ".default: " + value_error);
                        break;
                    }
                    spec.reload_impact = parse_reload_impact(property);

                    cJSON *minimum = cJSON_GetObjectItemCaseSensitive(property, "minimum");
                    cJSON *maximum = cJSON_GetObjectItemCaseSensitive(property, "maximum");
                    if (cJSON_IsNumber(minimum))
                    {
                        spec.has_minimum = true;
                        spec.minimum = minimum->valuedouble;
                    }
                    if (cJSON_IsNumber(maximum))
                    {
                        spec.has_maximum = true;
                        spec.maximum = maximum->valuedouble;
                    }

                    cJSON *enum_item = cJSON_GetObjectItemCaseSensitive(property, "enum");
                    if (cJSON_IsArray(enum_item))
                    {
                        cJSON *enum_value = nullptr;
                        cJSON_ArrayForEach(enum_value, enum_item)
                        {
                            LogicParameterValue parsed;
                            if (!parse_value(enum_value, spec.type, parsed, value_error))
                            {
                                fail(logic_name + "." + spec.key + ".enum: " + value_error);
                                break;
                            }
                            spec.enum_values.push_back(std::move(parsed));
                        }
                    }

                    schema.spec_index.emplace(spec.key, schema.ordered_specs.size());
                    schema.ordered_specs.push_back(std::move(spec));
                }
                if (!error_.empty())
                    break;
                if (!schemas_.emplace(logic_name, std::move(schema)).second)
                {
                    fail("duplicate embedded logic schema: " + logic_name);
                    break;
                }
            }
            if (!error_.empty())
                break;
        }

        cJSON_Delete(root);
    }

    std::unordered_map<std::string, LogicSchema> schemas_;
    std::string error_;
};

const LogicSchemaRegistry &schema_registry()
{
    static const LogicSchemaRegistry registry;
    return registry;
}

void add_error(std::vector<LogicParameterError> *errors, const std::string &field, const std::string &message)
{
    if (errors)
        errors->push_back({field, message});
}

bool parse_configured_value(cJSON *item, const ParameterSpec &spec, LogicParameterValue &out, std::string &error)
{
    /* 与 Registry 构造时相同的解析规则，单独写在这里避免把内部加载器暴露出去。 */
    out = LogicParameterValue();
    out.type = spec.type;
    switch (spec.type)
    {
    case LogicParameterType::STRING:
        if (!cJSON_IsString(item) || !item->valuestring)
        {
            error = "必须是字符串";
            return false;
        }
        out.text_value = item->valuestring;
        break;
    case LogicParameterType::NUMBER:
        if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble))
        {
            error = "必须是有限数值";
            return false;
        }
        out.number_value = item->valuedouble;
        break;
    case LogicParameterType::INTEGER:
        if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
            std::floor(item->valuedouble) != item->valuedouble || item->valuedouble < -kMaxExactJsonInteger ||
            item->valuedouble > kMaxExactJsonInteger)
        {
            error = "必须是整数";
            return false;
        }
        out.integer_value = static_cast<int64_t>(item->valuedouble);
        break;
    case LogicParameterType::BOOLEAN:
        if (!cJSON_IsBool(item))
        {
            error = "必须是布尔值";
            return false;
        }
        out.bool_value = cJSON_IsTrue(item);
        break;
    case LogicParameterType::ARRAY:
    case LogicParameterType::OBJECT: {
        const bool valid = spec.type == LogicParameterType::ARRAY ? cJSON_IsArray(item) : cJSON_IsObject(item);
        if (!valid)
        {
            error = spec.type == LogicParameterType::ARRAY ? "必须是数组" : "必须是对象";
            return false;
        }
        char *text = cJSON_PrintUnformatted(item);
        if (!text)
        {
            error = "JSON 序列化失败";
            return false;
        }
        out.text_value = text;
        cJSON_free(text);
        break;
    }
    }

    const double numeric =
        spec.type == LogicParameterType::NUMBER ? out.number_value : static_cast<double>(out.integer_value);
    if ((spec.type == LogicParameterType::NUMBER || spec.type == LogicParameterType::INTEGER) && spec.has_minimum &&
        numeric < spec.minimum)
    {
        error = "不得小于 " + std::to_string(spec.minimum);
        return false;
    }
    if ((spec.type == LogicParameterType::NUMBER || spec.type == LogicParameterType::INTEGER) && spec.has_maximum &&
        numeric > spec.maximum)
    {
        error = "不得大于 " + std::to_string(spec.maximum);
        return false;
    }
    if (!spec.enum_values.empty() &&
        std::find(spec.enum_values.begin(), spec.enum_values.end(), out) == spec.enum_values.end())
    {
        error = "不在允许的枚举值中";
        return false;
    }
    return true;
}

cJSON *value_to_json(const LogicParameterValue &value)
{
    switch (value.type)
    {
    case LogicParameterType::STRING:
        return cJSON_CreateString(value.text_value.c_str());
    case LogicParameterType::NUMBER:
        return cJSON_CreateNumber(value.number_value);
    case LogicParameterType::INTEGER:
        return cJSON_CreateNumber(static_cast<double>(value.integer_value));
    case LogicParameterType::BOOLEAN:
        return cJSON_CreateBool(value.bool_value ? 1 : 0);
    case LogicParameterType::ARRAY:
    case LogicParameterType::OBJECT:
        return cJSON_Parse(value.text_value.c_str());
    }
    return nullptr;
}
} // namespace

bool operator==(const LogicParameterValue &lhs, const LogicParameterValue &rhs)
{
    if (lhs.type != rhs.type)
        return false;
    switch (lhs.type)
    {
    case LogicParameterType::STRING:
        return lhs.text_value == rhs.text_value;
    case LogicParameterType::ARRAY:
    case LogicParameterType::OBJECT: {
        cJSON *left = cJSON_Parse(lhs.text_value.c_str());
        cJSON *right = cJSON_Parse(rhs.text_value.c_str());
        const bool equal = left && right && cJSON_Compare(left, right, 1);
        cJSON_Delete(left);
        cJSON_Delete(right);
        return equal;
    }
    case LogicParameterType::NUMBER:
        return lhs.number_value == rhs.number_value;
    case LogicParameterType::INTEGER:
        return lhs.integer_value == rhs.integer_value;
    case LogicParameterType::BOOLEAN:
        return lhs.bool_value == rhs.bool_value;
    }
    return false;
}

const LogicParameterValue *LogicParameterSet::find(const std::string &key) const
{
    const auto it = values_.find(key);
    return it == values_.end() ? nullptr : &it->second;
}

void LogicParameterSet::assign(std::string key, LogicParameterValue value)
{
    values_[std::move(key)] = std::move(value);
}

bool LogicParameterSet::has(const char *key) const
{
    return key && find(key) != nullptr;
}

float LogicParameterSet::get_float(const char *key) const
{
    const LogicParameterValue *value = key ? find(key) : nullptr;
    if (!value)
        return 0.0f;
    if (value->type == LogicParameterType::NUMBER)
        return static_cast<float>(value->number_value);
    if (value->type == LogicParameterType::INTEGER)
        return static_cast<float>(value->integer_value);
    return 0.0f;
}

int64_t LogicParameterSet::get_int(const char *key) const
{
    const LogicParameterValue *value = key ? find(key) : nullptr;
    if (!value)
        return 0;
    if (value->type == LogicParameterType::INTEGER)
        return value->integer_value;
    return 0;
}

bool LogicParameterSet::get_bool(const char *key) const
{
    const LogicParameterValue *value = key ? find(key) : nullptr;
    return value && value->type == LogicParameterType::BOOLEAN ? value->bool_value : false;
}

std::string LogicParameterSet::get_string(const char *key) const
{
    const LogicParameterValue *value = key ? find(key) : nullptr;
    return value && value->type == LogicParameterType::STRING ? value->text_value : std::string();
}

std::string LogicParameterSet::get_json(const char *key) const
{
    const LogicParameterValue *value = key ? find(key) : nullptr;
    if (!value || (value->type != LogicParameterType::ARRAY && value->type != LogicParameterType::OBJECT))
        return std::string();
    return value->text_value;
}

bool logic_parameters_resolve(const std::string &logic_name, const std::string &parameters_json,
                              std::string *normalized_json, LogicParameterSet *set_out,
                              std::vector<LogicParameterError> *errors)
{
    if (errors)
        errors->clear();
    const LogicSchemaRegistry &registry = schema_registry();
    if (!registry.valid())
    {
        add_error(errors, "logic_parameters", registry.error());
        return false;
    }
    const LogicSchema *schema = registry.find(logic_name);
    if (!schema)
    {
        add_error(errors, "logic", "未找到已编译的模块 Schema: " + logic_name);
        return false;
    }

    const std::string input_text = parameters_json.empty() ? "{}" : parameters_json;
    cJSON *input = cJSON_Parse(input_text.c_str());
    if (!cJSON_IsObject(input))
    {
        add_error(errors, "logic_parameters", "必须是 JSON 对象");
        cJSON_Delete(input);
        return false;
    }

    bool valid = true;
    std::unordered_set<std::string> input_keys;
    for (cJSON *item = input->child; item; item = item->next)
    {
        const std::string key = item->string ? item->string : "";
        if (!input_keys.insert(key).second)
        {
            add_error(errors, "logic_parameters." + key, "参数重复");
            valid = false;
        }
    }
    if (!schema->additional_properties)
    {
        for (cJSON *item = input->child; item; item = item->next)
        {
            const std::string key = item->string ? item->string : "";
            if (schema->spec_index.find(key) == schema->spec_index.end())
            {
                add_error(errors, "logic_parameters." + key, "未知参数");
                valid = false;
            }
        }
    }

    LogicParameterSet resolved;
    cJSON *normalized = cJSON_CreateObject();
    for (const auto &spec : schema->ordered_specs)
    {
        cJSON *configured = cJSON_GetObjectItemCaseSensitive(input, spec.key.c_str());
        LogicParameterValue value = spec.default_value;
        if (configured)
        {
            std::string error;
            if (!parse_configured_value(configured, spec, value, error))
            {
                add_error(errors, "logic_parameters." + spec.key, error);
                valid = false;
                continue;
            }
        }
        resolved.assign(spec.key, value);
        cJSON *json_value = value_to_json(value);
        if (!json_value)
        {
            add_error(errors, "logic_parameters." + spec.key, "无法规范化参数值");
            valid = false;
            continue;
        }
        cJSON_AddItemToObject(normalized, spec.key.c_str(), json_value);
    }

    if (valid)
    {
        if (normalized_json)
        {
            char *text = cJSON_PrintUnformatted(normalized);
            if (!text)
            {
                add_error(errors, "logic_parameters", "无法生成规范参数 JSON");
                valid = false;
            }
            else
            {
                *normalized_json = text;
                cJSON_free(text);
            }
        }
        if (valid && set_out)
            *set_out = std::move(resolved);
    }

    cJSON_Delete(normalized);
    cJSON_Delete(input);
    return valid;
}

LogicReloadImpact logic_parameters_reload_impact(const std::string &logic_name, const std::string &old_parameters_json,
                                                 const std::string &new_parameters_json,
                                                 std::vector<std::string> *changed_keys)
{
    if (changed_keys)
        changed_keys->clear();
    const LogicSchemaRegistry &registry = schema_registry();
    const LogicSchema *schema = registry.find(logic_name);
    if (!schema)
        return LogicReloadImpact::RESTART_REQUIRED;

    LogicParameterSet old_set, new_set;
    if (!logic_parameters_resolve(logic_name, old_parameters_json, nullptr, &old_set, nullptr) ||
        !logic_parameters_resolve(logic_name, new_parameters_json, nullptr, &new_set, nullptr))
        return LogicReloadImpact::RESTART_REQUIRED;

    LogicReloadImpact impact = LogicReloadImpact::NONE;
    for (const auto &spec : schema->ordered_specs)
    {
        const LogicParameterValue *old_value = old_set.find(spec.key);
        const LogicParameterValue *new_value = new_set.find(spec.key);
        if ((!old_value && !new_value) || (old_value && new_value && *old_value == *new_value))
            continue;
        if (changed_keys)
            changed_keys->push_back(spec.key);
        if (static_cast<int>(spec.reload_impact) > static_cast<int>(impact))
            impact = spec.reload_impact;
    }
    return impact;
}

const char *logic_reload_impact_name(LogicReloadImpact impact)
{
    switch (impact)
    {
    case LogicReloadImpact::NONE:
        return "none";
    case LogicReloadImpact::PRESERVE_STATE:
        return "preserve_state";
    case LogicReloadImpact::RESET_STATE:
        return "reset_state";
    case LogicReloadImpact::RESTART_REQUIRED:
        return "restart_required";
    }
    return "unknown";
}
