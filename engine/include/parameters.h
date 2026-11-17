/**
 * @file parameters.h
 * @brief Schema 驱动的通道/全局逻辑专有参数。
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

/** @brief 判断类型及值是否相等；数组/对象按 JSON 内容比较，对象键顺序不影响结果。
 * 示例：
 * @code
 * LogicParameterValue a, b;
 * bool result = (a == b);
 * @endcode
 */
bool operator==(const LogicParameterValue &lhs, const LogicParameterValue &rhs);
/** @brief 判断类型及值是否不相等；数组/对象按 JSON 内容比较，对象键顺序不影响结果。
 * 示例：
 * @code
 * LogicParameterValue a, b;
 * bool result = (a != b);
 * @endcode
 */
inline bool operator!=(const LogicParameterValue &lhs, const LogicParameterValue &rhs)
{
    return !(lhs == rhs);
}

class LogicParameterSet
{
  public:
    /** @brief 检查解析后参数表是否含 key；空指针返回 false。
     * 示例：
     * @code
     * LogicParameterSet params;
     * bool present = params.has("count");
     * @endcode
     */
    bool has(const char *key) const;
    /** @brief 读取 number/integer 为 float；缺失、类型不符或超出有限 float 范围返回 0。
     * 示例：
     * @code
     * LogicParameterSet params;
     * auto value = params.get_float("score");
     * @endcode
     */
    float get_float(const char *key) const;
    /** @brief 读取 integer；不把 number 自动转成整数，失败返回 0。
     * 示例：
     * @code
     * LogicParameterSet params;
     * auto value = params.get_int("count");
     * @endcode
     */
    int64_t get_int(const char *key) const;
    /** @brief 读取 boolean；缺失或类型不符返回 false。
     * 示例：
     * @code
     * LogicParameterSet params;
     * auto value = params.get_bool("enabled");
     * @endcode
     */
    bool get_bool(const char *key) const;
    /** @brief 读取 string；缺失或类型不符返回空字符串。
     * 示例：
     * @code
     * LogicParameterSet params;
     * auto value = params.get_string("label");
     * @endcode
     */
    std::string get_string(const char *key) const;
    /** @brief 读取 array/object 的 JSON 文本；缺失或类型不符返回空字符串。
     * 示例：
     * @code
     * LogicParameterSet params;
     * auto value = params.get_json("zones");
     * @endcode
     */
    std::string get_json(const char *key) const;

    /** @brief 借用参数值，未找到返回 nullptr；只在参数表未被替换/销毁时使用。
     * 示例：
     * @code
     * LogicParameterSet params;
     * const auto *value = params.find("count");
     * @endcode
     */
    const LogicParameterValue *find(const std::string &key) const;
    /** @brief 直接写入类型化参数并覆盖同名项；不执行 Schema 校验，业务优先用 resolve。
     * 示例：
     * @code
     * LogicParameterSet params;
     * LogicParameterValue v;
     * v.type = LogicParameterType::INTEGER;
     * v.integer_value = 3;
     * params.assign("count", v);
     * @endcode
     */
    void assign(std::string key, LogicParameterValue value);

  private:
    std::unordered_map<std::string, LogicParameterValue> values_;
};

struct LogicParameterError
{
    std::string field;
    std::string message;
};

/** @brief 按已注册 Schema 校验、补默认值并生成参数表；空 logic_name 仅接受空对象。
 * @return 成功 true；失败 false 且保持 normalized_json/set_out 不变；输出指针可为空，errors 每次清空。
 * 示例：
 * @code
 * LogicParameterSet params;
 * std::string normalized;
 * bool ok = logic_parameters_resolve("", "{}", &normalized, &params, nullptr);
 * @endcode
 */
bool logic_parameters_resolve(const std::string &logic_name, const std::string &parameters_json,
                              std::string *normalized_json, LogicParameterSet *set_out,
                              std::vector<LogicParameterError> *errors);

/** @brief 比较参数变更并返回最高热重载级别；changed_keys 可为空，有值时先清空再写。
 * @return 不变为 NONE；未知模块或任一参数无效为 RESTART_REQUIRED。
 * 示例：
 * @code
 * auto impact = logic_parameters_reload_impact("", "{}", "{}");
 * @endcode
 */
LogicReloadImpact logic_parameters_reload_impact(const std::string &logic_name, const std::string &old_parameters_json,
                                                 const std::string &new_parameters_json,
                                                 std::vector<std::string> *changed_keys = nullptr);

/** @brief 将热重载级别转为稳定英文名称；未知枚举返回 "unknown"，无需释放。
 * 示例：
 * @code
 * const char *name = logic_reload_impact_name(LogicReloadImpact::RESET_STATE);
 * @endcode
 */
const char *logic_reload_impact_name(LogicReloadImpact impact);

namespace vision
{
/** @brief 告诉程序各业务函数允许哪些参数、默认值是多少；在启动前调用。
 * 未调用时，已注册的函数默认只接受空参数 {}，适合无参数的独立测试。
 * 调用后会严格按提供的目录检查，目录里缺少的函数也会被拒绝。
 * @note 无效内容抛 invalid_argument；解析开始后再注册抛 logic_error；复制并解析输入字符串。
 * 示例：
 * @code
 * vision::set_logic_catalog(R"({"channel_logics":[],"global_logics":[]})");
 * @endcode
 */
void set_logic_catalog(const std::string &catalog_json);
} // namespace vision
