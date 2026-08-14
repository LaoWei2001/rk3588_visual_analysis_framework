/**
 * @file logic_outputs.h
 * @brief 通道 logic 向全局 logic 发布的类型化、同帧业务变量。
 *
 * ChannelContext 每次执行时持有一份新的 LogicOutputSet。通道 logic 通过
 * publish_*() 写入，框架在 logic 返回后把它与 frame/results 一起原子发布。
 * 全局 logic 通过 ChannelLogicSnapshot 读取，不需要也不允许依赖其它模块的私有状态。
 */
#pragma once

#include <cstddef>
#include <cstdint>
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
    bool has(const char *key) const
    {
        return key && values_.find(key) != values_.end();
    }

    const LogicOutputValue *find(const char *key) const
    {
        if (!key)
            return nullptr;
        const auto it = values_.find(key);
        return it == values_.end() ? nullptr : &it->second;
    }

    bool empty() const
    {
        return values_.empty();
    }

    std::size_t size() const
    {
        return values_.size();
    }

    void set_string(const char *key, const std::string &value)
    {
        LogicOutputValue item;
        item.type = LogicOutputType::STRING;
        item.text_value = value;
        assign(key, std::move(item));
    }

    void set_number(const char *key, double value)
    {
        LogicOutputValue item;
        item.type = LogicOutputType::NUMBER;
        item.number_value = value;
        assign(key, std::move(item));
    }

    void set_int(const char *key, int64_t value)
    {
        LogicOutputValue item;
        item.type = LogicOutputType::INTEGER;
        item.integer_value = value;
        assign(key, std::move(item));
    }

    void set_bool(const char *key, bool value)
    {
        LogicOutputValue item;
        item.type = LogicOutputType::BOOLEAN;
        item.bool_value = value;
        assign(key, std::move(item));
    }

    void set_json(const char *key, const std::string &json)
    {
        LogicOutputValue item;
        item.type = LogicOutputType::JSON;
        item.text_value = json;
        assign(key, std::move(item));
    }

    /* key 缺失、out 为空或类型不完全匹配均返回 false；合法的 0/false/空字符串仍返回 true。 */
    bool try_get_string(const char *key, std::string *out) const
    {
        const LogicOutputValue *item = find(key);
        if (!out || !item || item->type != LogicOutputType::STRING)
            return false;
        *out = item->text_value;
        return true;
    }

    bool try_get_number(const char *key, double *out) const
    {
        const LogicOutputValue *item = find(key);
        if (!out || !item || item->type != LogicOutputType::NUMBER)
            return false;
        *out = item->number_value;
        return true;
    }

    bool try_get_int(const char *key, int64_t *out) const
    {
        const LogicOutputValue *item = find(key);
        if (!out || !item || item->type != LogicOutputType::INTEGER)
            return false;
        *out = item->integer_value;
        return true;
    }

    bool try_get_bool(const char *key, bool *out) const
    {
        const LogicOutputValue *item = find(key);
        if (!out || !item || item->type != LogicOutputType::BOOLEAN)
            return false;
        *out = item->bool_value;
        return true;
    }

    bool try_get_json(const char *key, std::string *out) const
    {
        const LogicOutputValue *item = find(key);
        if (!out || !item || item->type != LogicOutputType::JSON)
            return false;
        *out = item->text_value;
        return true;
    }

    const std::unordered_map<std::string, LogicOutputValue> &values() const
    {
        return values_;
    }

  private:
    void assign(const char *key, LogicOutputValue value)
    {
        if (key && *key)
            values_[key] = std::move(value);
    }

    std::unordered_map<std::string, LogicOutputValue> values_;
};
