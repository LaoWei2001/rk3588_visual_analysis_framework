#pragma once

#include <initializer_list>
#include <map>
#include <string>
#include <type_traits>

struct ChannelContext;
struct GlobalContext;

class EventValue
{
  public:
    enum Type
    {
        STRING,
        NUMBER,
        BOOLEAN,
        JSON
    };

    EventValue() = default;
    explicit EventValue(const std::string &value, Type type = STRING) : type_(type), text_(value)
    {
    }
    explicit EventValue(double value) : type_(NUMBER), number_(value)
    {
    }
    explicit EventValue(bool value) : type_(BOOLEAN), boolean_(value)
    {
    }

    Type type() const
    {
        return type_;
    }
    const std::string &text() const
    {
        return text_;
    }
    double number() const
    {
        return number_;
    }
    bool boolean() const
    {
        return boolean_;
    }

  private:
    Type type_ = STRING;
    std::string text_;
    double number_ = 0.0;
    bool boolean_ = false;
};

struct EventField
{
    EventField(const std::string &field_key, const EventValue &field_value) : key(field_key), value(field_value)
    {
    }

    std::string key;
    EventValue value;
};

class EventFields
{
  public:
    EventFields() = default;
    EventFields(std::initializer_list<EventField> fields)
    {
        for (const EventField &field : fields)
            set_value(field.key, field.value);
    }

    void set_value(const std::string &key, const EventValue &value)
    {
        values_[key] = value;
    }
    void set_string(const std::string &key, const std::string &value)
    {
        values_[key] = EventValue(value);
    }
    void set_number(const std::string &key, double value)
    {
        values_[key] = EventValue(value);
    }
    void set_bool(const std::string &key, bool value)
    {
        values_[key] = EventValue(value);
    }
    void set_json(const std::string &key, const std::string &json)
    {
        values_[key] = EventValue(json, EventValue::JSON);
    }
    const std::map<std::string, EventValue> &values() const
    {
        return values_;
    }

  private:
    std::map<std::string, EventValue> values_;
};

inline EventField event_field(const std::string &key, const std::string &value)
{
    return EventField(key, EventValue(value));
}

inline EventField event_field(const std::string &key, const char *value)
{
    return EventField(key, EventValue(value ? std::string(value) : std::string()));
}

inline EventField event_field(const std::string &key, bool value)
{
    return EventField(key, EventValue(value));
}

template <typename T>
inline typename std::enable_if<std::is_arithmetic<T>::value &&
                                   !std::is_same<typename std::decay<T>::type, bool>::value,
                               EventField>::type
event_field(const std::string &key, T value)
{
    return EventField(key, EventValue(static_cast<double>(value)));
}

inline EventField event_json_field(const std::string &key, const std::string &json)
{
    return EventField(key, EventValue(json, EventValue::JSON));
}

enum class EventMergeMode
{
    POLICY,
    NEVER
};

/*
 * logic 只描述业务事件，不选择媒体、上传目标或远端接口字段。
 * 图片、视频、纯 JSON 和适配器均由当前通道 report_policy.deliveries 决定。
 */
struct EventRequest
{
    std::string event_type;
    std::string message;
    EventFields fields;
    EventMergeMode merge_mode = EventMergeMode::NEVER;
    /* 全局 logic 可为本次事件选择来源标识和单通道图片；-1 使用节点配置的默认通道。
     * 有画布输入时图片拼接连入通道；没有画布输入时图片使用该来源通道。
     * 事件视频始终使用全局上报节点明确选择的 media_source_channel_id，以限制预录开销。
     * 通道 logic 调用时无需设置，仍使用 ctx->chnId。 */
    int source_channel_id = -1;
};

enum class EventReportStatus
{
    CREATED,
    MERGED,
    CREATED_MEDIA_FAILED,
    DISABLED,
    INVALID_REQUEST,
    NO_DELIVERY,
    WORKER_UNAVAILABLE,
    STORAGE_ERROR
};

struct EventReportResult
{
    EventReportStatus status = EventReportStatus::INVALID_REQUEST;
    std::string event_id;
    std::string detail;

    bool accepted() const
    {
        return status == EventReportStatus::CREATED || status == EventReportStatus::MERGED ||
               status == EventReportStatus::CREATED_MEDIA_FAILED;
    }

    bool media_failed() const
    {
        return status == EventReportStatus::CREATED_MEDIA_FAILED;
    }

    explicit operator bool() const
    {
        return accepted();
    }
};

const char *event_report_status_name(EventReportStatus status);

/*
 * 唯一事件提交入口。函数只创建本地持久化事件，不直接联网；远端投递结果由上传服务维护。
 * accepted() 表示事件已进入本地持久化队列，不表示磁盘落盘或远端已经接收。
 */
EventReportResult report_event(ChannelContext *ctx, const EventRequest &request);
EventReportResult report_event(GlobalContext *ctx, const EventRequest &request);

/* 录像模块完成 MP4 后调用。 */
void event_report_video_ready(const std::string &event_id, const std::string &video_path);
void event_report_video_failed(const std::string &event_id, const std::string &video_path,
                               const std::string &reason);

/* 排空并停止事件持久化/图片落盘线程。 */
void event_report_deinit(void);
