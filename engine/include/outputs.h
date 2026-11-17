/**
 * @file outputs.h
 * @brief 通道 logic 向全局 logic 发布的类型化、同帧业务变量。
 *
 * ChannelContext 每次执行时持有一份新的 LogicOutputSet。通道 logic 通过
 * publish_*() 写入，框架在 logic 返回后把它与 frame/results 一起原子发布。
 * 全局 logic 通过 GlobalContext::input() 返回的 ChannelInput 读取，不需要也不允许依赖其它模块的私有状态。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

enum class LogicOutputType
{
    STRING,
    NUMBER,
    INTEGER,
    BOOLEAN,
    JSON
};

struct LogicOutputValue
{
    LogicOutputType type = LogicOutputType::STRING;
    std::string text_value;
    double number_value = 0.0;
    int64_t integer_value = 0;
    bool bool_value = false;
};

class LogicOutputSet
{
  public:
    /** @brief 检查字段是否存在；空 key 返回 false，不检查字段类型。
     * 示例：
     * @code
     * LogicOutputSet values;
     * bool present = values.has("count");
     * @endcode
     */
    bool has(const char *key) const
    {
        return key && values_.find(key) != values_.end();
    }

    /** @brief 借用字段值；未找到/空 key 返回 nullptr，容器销毁或替换后勿再使用。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_int("count", 3);
     * const auto *v = values.find("count");
     * @endcode
     */
    const LogicOutputValue *find(const char *key) const
    {
        if (!key)
            return nullptr;
        const auto it = values_.find(key);
        return it == values_.end() ? nullptr : &it->second;
    }

    /** @brief 判断输出表是否没有任何字段。
     * 示例：
     * @code
     * LogicOutputSet values;
     * bool none = values.empty();
     * @endcode
     */
    bool empty() const
    {
        return values_.empty();
    }

    /** @brief 返回不同输出字段的数量，同名覆盖不增加数量。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_int("count", 3);
     * auto n = values.size();
     * @endcode
     */
    std::size_t size() const
    {
        return values_.size();
    }

    /** @brief 复制字符串，同名覆盖，空 key 忽略。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_string("value", "person");
     * @endcode
     */
    void set_string(const char *key, const std::string &value)
    {
        LogicOutputValue item;
        item.type = LogicOutputType::STRING;
        item.text_value = value;
        assign(key, std::move(item));
    }

    /** @brief 写入 double 数值，同名覆盖，空 key 忽略。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_number("value", 0.9);
     * @endcode
     */
    void set_number(const char *key, double value)
    {
        LogicOutputValue item;
        item.type = LogicOutputType::NUMBER;
        item.number_value = value;
        assign(key, std::move(item));
    }

    /** @brief 写入 int64 整数，同名覆盖，空 key 忽略。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_int("value", 3);
     * @endcode
     */
    void set_int(const char *key, int64_t value)
    {
        LogicOutputValue item;
        item.type = LogicOutputType::INTEGER;
        item.integer_value = value;
        assign(key, std::move(item));
    }

    /** @brief 写入布尔值，同名覆盖，空 key 忽略。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_bool("value", true);
     * @endcode
     */
    void set_bool(const char *key, bool value)
    {
        LogicOutputValue item;
        item.type = LogicOutputType::BOOLEAN;
        item.bool_value = value;
        assign(key, std::move(item));
    }

    /** @brief 复制写入JSON 文本，同名覆盖，空 key 忽略。不解析 JSON，调用方保证语法有效。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_json("value", "[1,2]");
     * @endcode
     */
    void set_json(const char *key, const std::string &json)
    {
        LogicOutputValue item;
        item.type = LogicOutputType::JSON;
        item.text_value = json;
        assign(key, std::move(item));
    }

    /** @brief 严格读取字符串；成功返回 true，缺失/类型不符/空 out 时 false 且不改 out。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_string("value", "person");
     * std::string out{};
     * bool ok = values.try_get_string("value", &out);
     * @endcode
     */
    bool try_get_string(const char *key, std::string *out) const
    {
        const LogicOutputValue *item = find(key);
        if (!out || !item || item->type != LogicOutputType::STRING)
            return false;
        *out = item->text_value;
        return true;
    }

    /** @brief 严格读取double 数值；成功返回 true，缺失/类型不符/空 out 时 false 且不改 out。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_number("value", 0.9);
     * double out{};
     * bool ok = values.try_get_number("value", &out);
     * @endcode
     */
    bool try_get_number(const char *key, double *out) const
    {
        const LogicOutputValue *item = find(key);
        if (!out || !item || item->type != LogicOutputType::NUMBER)
            return false;
        *out = item->number_value;
        return true;
    }

    /** @brief 严格读取int64 整数；成功返回 true，缺失/类型不符/空 out 时 false 且不改 out。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_int("value", 3);
     * int64_t out{};
     * bool ok = values.try_get_int("value", &out);
     * @endcode
     */
    bool try_get_int(const char *key, int64_t *out) const
    {
        const LogicOutputValue *item = find(key);
        if (!out || !item || item->type != LogicOutputType::INTEGER)
            return false;
        *out = item->integer_value;
        return true;
    }

    /** @brief 严格读取布尔值；成功返回 true，缺失/类型不符/空 out 时 false 且不改 out。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_bool("value", true);
     * bool out{};
     * bool ok = values.try_get_bool("value", &out);
     * @endcode
     */
    bool try_get_bool(const char *key, bool *out) const
    {
        const LogicOutputValue *item = find(key);
        if (!out || !item || item->type != LogicOutputType::BOOLEAN)
            return false;
        *out = item->bool_value;
        return true;
    }

    /** @brief 严格读取JSON 文本；成功返回 true，缺失/类型不符/空 out 时 false 且不改 out。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_json("value", "[1,2]");
     * std::string out{};
     * bool ok = values.try_get_json("value", &out);
     * @endcode
     */
    bool try_get_json(const char *key, std::string *out) const
    {
        const LogicOutputValue *item = find(key);
        if (!out || !item || item->type != LogicOutputType::JSON)
            return false;
        *out = item->text_value;
        return true;
    }

    /** @brief 借用完整只读输出表；遍历时不要修改或销毁容器。
     * 示例：
     * @code
     * LogicOutputSet values;
     * for (const auto &entry : values.values())
     * {
     *     (void)entry.first;
     * }
     * @endcode
     */
    const std::unordered_map<std::string, LogicOutputValue> &values() const
    {
        return values_;
    }

  private:
    /** @brief 内部统一写入；空指针或空名称忽略，同名值替换。
     * 示例：
     * @code
     * LogicOutputSet values;
     * values.set_int("count", 3); // 外部使用类型化 setter
     * @endcode
     */
    void assign(const char *key, LogicOutputValue value)
    {
        if (key && *key)
            values_[key] = std::move(value);
    }

    std::unordered_map<std::string, LogicOutputValue> values_;
};
