/**
 * @file logic_parameters.h
 * @brief Schema 驱动的通道逻辑专有参数。
 *
 * 每个模块的 logic.json 是参数类型、默认值、范围和热重载策略的唯一真源。
 * 构建脚本把聚合后的 Schema 嵌入二进制；配置只保存 logic_parameters 对象。
 */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

enum class LogicParameterType
{
    STRING,
    NUMBER,
    INTEGER,
    BOOLEAN,
    ARRAY,
    OBJECT
};

enum class LogicReloadImpact
{
    NONE = 0,
    PRESERVE_STATE = 1,
    RESET_STATE = 2,
    RESTART_REQUIRED = 3
};

struct LogicParameterValue
{
    LogicParameterType type = LogicParameterType::STRING;
    double number_value = 0.0;
    int64_t integer_value = 0;
    bool bool_value = false;
    std::string text_value; /* string 值，或 array/object 的规范 JSON */
};

bool operator==(const LogicParameterValue &lhs, const LogicParameterValue &rhs);
inline bool operator!=(const LogicParameterValue &lhs, const LogicParameterValue &rhs)
{
    return !(lhs == rhs);
}

class LogicParameterSet
{
  public:
    bool has(const char *key) const;
    float get_float(const char *key) const;
    int64_t get_int(const char *key) const;
    bool get_bool(const char *key) const;
    std::string get_string(const char *key) const;
    std::string get_json(const char *key) const;

    const LogicParameterValue *find(const std::string &key) const;
    void assign(std::string key, LogicParameterValue value);

  private:
    std::unordered_map<std::string, LogicParameterValue> values_;
};

struct LogicParameterError
{
    std::string field;
    std::string message;
};

/**
 * 校验参数、填入 Schema 默认值并生成稳定 JSON/类型化参数表。
 * logic_name 为空表示无后处理模块，此时使用只接受 `{}` 的框架内置空 Schema。
 * normalized_json/set_out 均可为空；错误时保持调用方旧运行快照不变。
 */
bool logic_parameters_resolve(const std::string &logic_name, const std::string &parameters_json,
                              std::string *normalized_json, LogicParameterSet *set_out,
                              std::vector<LogicParameterError> *errors);

/** 比较同一种 logic 的两份参数，并按变更字段的 Schema 策略返回最高影响级别。 */
LogicReloadImpact logic_parameters_reload_impact(const std::string &logic_name, const std::string &old_parameters_json,
                                                 const std::string &new_parameters_json,
                                                 std::vector<std::string> *changed_keys = nullptr);

const char *logic_reload_impact_name(LogicReloadImpact impact);
