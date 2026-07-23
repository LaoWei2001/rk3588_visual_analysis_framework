#pragma once

#include <initializer_list>
#include <map>
#include <string>
#include <type_traits>

struct ChannelContext;

class AlarmValue
{
public:
    enum Type { STRING, NUMBER, BOOLEAN, JSON };
    AlarmValue() = default;
    explicit AlarmValue(const std::string &v, Type type = STRING) : type_(type), text_(v) {}
    explicit AlarmValue(double v) : type_(NUMBER), number_(v) {}
    explicit AlarmValue(bool v) : type_(BOOLEAN), boolean_(v) {}
    Type type() const { return type_; }
    const std::string &text() const { return text_; }
    double number() const { return number_; }
    bool boolean() const { return boolean_; }
private:
    Type type_ = STRING;
    std::string text_;
    double number_ = 0.0;
    bool boolean_ = false;
};

class AlarmFields
{
public:
    void set_value(const std::string &key, const AlarmValue &value) { values_[key] = value; }
    void set_string(const std::string &key, const std::string &value) { values_[key] = AlarmValue(value); }
    void set_number(const std::string &key, double value) { values_[key] = AlarmValue(value); }
    void set_bool(const std::string &key, bool value) { values_[key] = AlarmValue(value); }
    void set_json(const std::string &key, const std::string &json) { values_[key] = AlarmValue(json, AlarmValue::JSON); }
    const std::map<std::string, AlarmValue> &values() const { return values_; }
private:
    std::map<std::string, AlarmValue> values_;
};

struct AlarmField
{
    AlarmField(const std::string &field_key, const AlarmValue &field_value)
        : key(field_key), value(field_value) {}

    std::string key;
    AlarmValue value;
};

inline AlarmField alarm_field(const std::string &key, const std::string &value)
{
    return AlarmField(key, AlarmValue(value));
}

inline AlarmField alarm_field(const std::string &key, const char *value)
{
    return AlarmField(key, AlarmValue(value ? std::string(value) : std::string()));
}

inline AlarmField alarm_field(const std::string &key, bool value)
{
    return AlarmField(key, AlarmValue(value));
}

template<typename T>
inline typename std::enable_if<
    std::is_arithmetic<T>::value && !std::is_same<typename std::decay<T>::type, bool>::value,
    AlarmField>::type
alarm_field(const std::string &key, T value)
{
    return AlarmField(key, AlarmValue(static_cast<double>(value)));
}

inline AlarmField alarm_json_field(const std::string &key, const std::string &json)
{
    return AlarmField(key, AlarmValue(json, AlarmValue::JSON));
}

struct AlarmRequest
{
    std::string type;
    std::string message;
    AlarmFields fields;

    /* 发件箱中的业务记录类型，供 Web 区分普通告警、SOP正常记录和SOP违规记录。 */
    std::string record_kind = "alarm";

    /* 正常SOP结果不采集媒体，只复用 report_policy 中的 Dify 连接和业务字段配置。 */
    bool dify_json_only = false;

    /* 每轮SOP结果必须保持一轮一条；普通连续告警仍可按策略窗口合并。 */
    bool merge_enabled = true;

};

/*
 * 业务逻辑唯一上报入口。业务代码只提交事件和运行时字段；图片/视频、目标地址、
 * 叠加方式、录像时间窗和字段映射全部由 Web 生成的 report_policy 决定。
 * 此函数非阻塞，成功返回 event_id，未配置投递或失败时返回空字符串。
 */
std::string alarm_report(ChannelContext *ctx, const AlarmRequest &request);

/* 任意业务逻辑使用的简化入口：字段名称和数量不固定，数值类型自动转换。 */
std::string report_alarm(ChannelContext *ctx,
                         const std::string &type,
                         const std::string &message,
                         std::initializer_list<AlarmField> fields);

/* 录像模块完成MP4后调用。 */
void alarm_report_video_ready(const std::string &event_id, const std::string &video_path);

/* 停止图片落盘线程。必须在销毁 ChannelContext 相关资源之后调用。 */
void alarm_report_deinit(void);
