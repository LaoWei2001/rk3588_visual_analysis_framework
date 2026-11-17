/** @file events.h
 * @brief 发送报警并附带人数、类别等业务信息。
 * 常用写法：填写 EventRequest，用 event_field() 加数据，再调用 report_event()。
 * 后续的记录、图片、视频和发送方式，由网页上的上报设置决定。
 */
#pragma once

#include <initializer_list>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

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

    /** @brief 创建一个空的事件值，之后可以给它赋值。
     * @return 得到一个值对象；刚创建时表示空字符串 ""。
     * 不需要传入参数。一般组装事件时直接用 event_field() 更方便。
     * 示例：
     * @code
     * EventValue value;
     * value = EventValue(std::string("person"));
     * @endcode
     */
    EventValue() = default;
    /** @brief 把一段文字放进事件值，例如类别名称；也可以存放 JSON 数组或对象。
     * @param value 要保存的文字，传 std::string 变量，或写成 std::string("person")。
     * @param type 普通文字用默认 STRING；JSON 数组或对象文字填 JSON，并确保内容是合法 JSON。
     * @return 得到保存了这段文字的值对象；修改原来的文字不会改变它。
     * 不要直接写 EventValue("person")，那会被当成 true；event_field("label", "person") 可以直接传文字。
     * 示例：
     * @code
     * EventValue label(std::string("person"));
     * EventValue ids(std::string("[1,2]"), EventValue::JSON);
     * @endcode
     */
    explicit EventValue(const std::string &value, Type type = STRING) : type_(type), text_(value)
    {
    }
    /** @brief 把一个数值放进事件，例如识别分数或距离。
     * @param value 普通数值，例如 0.95；整数可写成 3.0，或直接用 event_field("count", 3)。
     * @return 得到数值类型的事件值。
     * 不要传无穷大或无效数字；很长的整数编号请按文字保存，避免数字末尾失真。
     * 示例：
     * @code
     * EventValue score(0.95);
     * double number = score.number(); // 0.95
     * @endcode
     */
    explicit EventValue(double value) : type_(NUMBER), number_(value)
    {
    }
    /** @brief 把一个“是/否”结果放进事件，例如是否发现危险。
     * @param value true 表示是或开启，false 表示否或关闭。
     * @return 得到布尔类型的事件值，保留 true/false。
     * 示例：
     * @code
     * EventValue alarm(true);
     * bool active = alarm.boolean(); // true
     * @endcode
     */
    explicit EventValue(bool value) : type_(BOOLEAN), boolean_(value)
    {
    }

    /** @brief 查看一个事件值装的是文字、数字、开关还是 JSON，决定用哪个函数读取。
     * @return STRING=文字，NUMBER=数字，BOOLEAN=true/false，JSON=数组或对象文字。
     * 不需要传入参数。
     * 示例：
     * @code
     * EventValue value(0.95);
     * if (value.type() == EventValue::NUMBER)
     * {
     *     double score = value.number();
     * }
     * @endcode
     */
    Type type() const
    {
        return type_;
    }
    /** @brief 读取事件值里的文字或 JSON 内容。
     * @return 返回保存的文字；不会把数字自动转成文字，普通数字值读取这里得到空字符串。
     * 不需要传入参数；通常先确认 type() 是 STRING 或 JSON。要保留内容，像示例一样复制到 std::string。
     * 示例：
     * @code
     * EventValue value(std::string("person"));
     * std::string label = value.text(); // "person"
     * @endcode
     */
    const std::string &text() const
    {
        return text_;
    }
    /** @brief 读取事件值里的数字，例如识别分数。
     * @return 返回保存的数字；其他类型通常返回 0，所以应先检查 type()==NUMBER。
     * 不需要传入参数；不会把 "3" 这样的文字转成数字。
     * 示例：
     * @code
     * EventValue value(0.95);
     * double score = value.number(); // 0.95
     * @endcode
     */
    double number() const
    {
        return number_;
    }
    /** @brief 读取事件值里的“是/否”结果。
     * @return 返回保存的 true/false；其他类型通常返回 false，所以应先检查 type()==BOOLEAN。
     * 不需要传入参数；不会把数字 1 自动当成 true。
     * 示例：
     * @code
     * EventValue value(true);
     * bool alarm = value.boolean(); // true
     * @endcode
     */
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
    /** @brief 创建一项“名称 + 值”的事件数据，例如 count=3。
     * @param field_key 这项数据的名称，例如 "count"。
     * @param field_value 这项数据的值，已经用 EventValue 创建好。
     * @return 得到一个可加入 EventRequest.fields 的字段。
     * 通常直接写 event_field("count", 3) 更简单。
     * 示例：
     * @code
     * EventField field("count", EventValue(3.0));
     * @endcode
     */
    EventField(const std::string &field_key, const EventValue &field_value) : key(field_key), value(field_value)
    {
    }

    std::string key;
    EventValue value;
};

class EventFields
{
  public:
    /** @brief 创建一个空的事件数据表，随后可以逐项加入人数、分数等内容。
     * @return 得到一个没有任何字段的表。
     * 不需要传入参数。
     * 示例：
     * @code
     * EventFields fields;
     * fields.set_number("count", 3);
     * fields.set_bool("alarm", true);
     * @endcode
     */
    EventFields() = default;
    /** @brief 一次创建包含多项数据的事件表，适合给同一条报警补充信息。
     * @param fields 用大括号列出各个字段，可由 event_field() 创建。
     * @return 得到包含这些字段的表；同一名称出现多次时，保留最后一个值。
     * 示例：
     * @code
     * EventFields fields{
     *     event_field("count", 3),
     *     event_field("alarm", true)
     * };
     * @endcode
     */
    EventFields(std::initializer_list<EventField> fields)
    {
        for (const EventField &field : fields)
            set_value(field.key, field.value);
    }

    /** @brief 把一个已经创建好的 EventValue 放进事件表。
     * @param key 字段名，例如 "score"。
     * @param value 已经创建好的文字、数字、开关或 JSON 值。
     * @return 没有返回值；表中会保存一份值，同名字段会被替换。
     * 示例：
     * @code
     * EventFields fields;
     * EventValue score(0.95);
     * fields.set_value("score", score);
     * @endcode
     */
    void set_value(const std::string &key, const EventValue &value)
    {
        values_[key] = value;
    }
    /** @brief 加入一项文字数据，例如类别名称。
     * @param key 这项数据的名称，例如 "score"、"alarm" 或 "track_ids"。
     * @param value 要保存的文字。
     * @return 没有返回值；数据写入当前表，同名字段会被替换。
     * 示例：
     * @code
     * EventFields fields;
     * fields.set_string("label", "person");
     * @endcode
     */
    void set_string(const std::string &key, const std::string &value)
    {
        values_[key] = EventValue(value);
    }
    /** @brief 加入一项数值数据，例如人数或识别分数。
     * @param key 这项数据的名称，例如 "score"、"alarm" 或 "track_ids"。
     * @param value 普通数字；不要传无穷大或无效数字。
     * @return 没有返回值；数据写入当前表，同名字段会被替换。
     * 很长的整数编号请用 set_string 保存，避免数字末尾失真。
     * 示例：
     * @code
     * EventFields fields;
     * fields.set_number("score", 0.95);
     * @endcode
     */
    void set_number(const std::string &key, double value)
    {
        values_[key] = EventValue(value);
    }
    /** @brief 加入一项“是/否”数据，例如是否触发报警。
     * @param key 这项数据的名称，例如 "score"、"alarm" 或 "track_ids"。
     * @param value true 或 false。
     * @return 没有返回值；数据写入当前表，同名字段会被替换。
     * 示例：
     * @code
     * EventFields fields;
     * fields.set_bool("alarm", true);
     * @endcode
     */
    void set_bool(const std::string &key, bool value)
    {
        values_[key] = EventValue(value);
    }
    /** @brief 加入一组 JSON 数据，例如多个目标编号组成的数组。
     * @param key 这项数据的名称，例如 "score"、"alarm" 或 "track_ids"。
     * @param json 合法的 JSON 文字，例如 [1,2]；本函数不检查格式。
     * @return 没有返回值；数据写入当前表，同名字段会被替换。
     * 想把内容当普通文字保存时，用 set_string。
     * 示例：
     * @code
     * EventFields fields;
     * fields.set_json("track_ids", "[1,2]");
     * @endcode
     */
    void set_json(const std::string &key, const std::string &json)
    {
        values_[key] = EventValue(json, EventValue::JSON);
    }
    /** @brief 查看事件表中的全部字段，适合逐项打印或在测试中检查。
     * @return 返回只供读取的表；每一项 first 是字段名，second 是 EventValue。
     * 不需要传入参数；使用期间让原来的 EventFields 对象保持存在。
     * 示例：
     * @code
     * EventFields fields{event_field("count", 3)};
     * for (const auto &field : fields.values())
     * {
     *     std::string name = field.first; // 字段名
     *     EventValue::Type type = field.second.type(); // 值的类型
     * }
     * @endcode
     */
    const std::map<std::string, EventValue> &values() const
    {
        return values_;
    }

  private:
    std::map<std::string, EventValue> values_;
};

/** @brief 把一个文字变量做成事件字段，用来补充报警信息。
 * @param key 字段名，例如 "label"。
 * @param value 要保存的 std::string 文字变量。
 * @return 返回一个可加入 request.fields 的字段；修改原来的文字不会影响它。
 * 示例：
 * @code
 * std::string label = "person";
 * EventRequest request;
 * request.fields = {event_field("label", label)};
 * @endcode
 */
inline EventField event_field(const std::string &key, const std::string &value)
{
    return EventField(key, EventValue(value));
}

/** @brief 把直接写出的文字做成事件字段，例如标明目标类别。
 * @param key 字段名，例如 "label"。
 * @param value 文字，例如 "person"；传 nullptr 时保存为空字符串 ""。
 * @return 返回一个文字字段，可加入 request.fields。
 * 示例：
 * @code
 * EventRequest request;
 * request.fields = {event_field("label", "person")};
 * @endcode
 */
inline EventField event_field(const std::string &key, const char *value)
{
    return EventField(key, EventValue(value ? std::string(value) : std::string()));
}

/** @brief 把一个“是/否”结果做成事件字段。
 * @param key 字段名，例如 "alarm"。
 * @param value true 表示是，false 表示否。
 * @return 返回一个 true/false 字段，可加入 request.fields。
 * 示例：
 * @code
 * EventRequest request;
 * request.fields = {event_field("alarm", true)};
 * @endcode
 */
inline EventField event_field(const std::string &key, bool value)
{
    return EventField(key, EventValue(value));
}

/** @brief 把整数或小数做成事件字段，例如人数、距离或识别分数。
 * @param key 字段名，例如 "count" 或 "score"。
 * @param value 普通整数或小数；T 自动确定，不需要自己填写。
 * @return 返回一个数字字段，可加入 request.fields。
 * 很长的整数编号可能失真；这类编号可先 std::to_string(id)，再作为文字传入。
 * 示例：
 * @code
 * EventRequest request;
 * request.fields = {
 *     event_field("count", 3),
 *     event_field("score", 0.95)
 * };
 * @endcode
 */
template <typename T>
inline typename std::enable_if<std::is_arithmetic<T>::value && !std::is_same<typename std::decay<T>::type, bool>::value,
                               EventField>::type
event_field(const std::string &key, T value)
{
    return EventField(key, EventValue(static_cast<double>(value)));
}

/** @brief 把 JSON 数组或对象做成事件字段，用来发送一组相关数据。
 * @param key 字段名，例如 "track_ids"。
 * @param json 合法的 JSON 文字，例如 [1,2]；本函数不检查格式。
 * @return 返回一个 JSON 字段；保存的是数组/对象内容，不是普通文字标签。
 * 示例：
 * @code
 * EventRequest request;
 * request.fields = {event_json_field("track_ids", "[1,2]")};
 * @endcode
 */
inline EventField event_json_field(const std::string &key, const std::string &json)
{
    return EventField(key, EventValue(json, EventValue::JSON));
}

enum class EventMergeMode
{
    POLICY, /* 按上报设置决定是否合入已有事件。 */
    NEVER   /* 每次调用单独提交一个事件（默认）。 */
};

/* 填写你要报告的事实；截图、录像和发送方式按网页中的上报设置处理。 */
struct EventRequest
{
    std::string event_type; /* 必填，对应本模块 logic.json 中 event_types 的 id。 */
    std::string message;    /* 给用户看的说明，可留空，例如“发现人员进入”。 */
    EventFields fields;     /* 额外业务数据，例如人数、类别和识别分数，可不填。 */
    EventMergeMode merge_mode = EventMergeMode::NEVER;
    /* 仅供全局逻辑指定事件来自哪一路；多通道共同触发时保留 -1，普通通道逻辑不用填。 */
    int source_channel_id = -1;

    /* 全局逻辑可填本次参与报警判断的通道 ID，普通通道逻辑不用填。
     * 最后使用哪些通道的图片，仍按网页上报设置选择。 */
    std::vector<int> evidence_channel_ids;
};

enum class EventReportStatus
{
    CREATED,              /* 新的报警已接收。 */
    MERGED,               /* 本次内容已合入原有报警。 */
    CREATED_MEDIA_FAILED, /* 报警已接收，但图片或视频准备失败。 */
    DISABLED,             /* 上报功能已关闭。 */
    INVALID_REQUEST,      /* 参数不正确，或当前接收器不支持此类事件。 */
    NO_DELIVERY,          /* 没有可用的上报方式。 */
    WORKER_UNAVAILABLE,   /* 处理事件的功能暂时不可用。 */
    STORAGE_ERROR         /* 无法提交事件保存任务。 */
};

struct EventReportResult
{
    EventReportStatus status = EventReportStatus::INVALID_REQUEST;
    std::string event_id; /* 此事件的编号；失败时可能为空。 */
    std::string detail;   /* 提交结果的说明，失败时先查看这里。 */

    /** @brief 检查刚才提交的事件有没有被接收，方便决定是否记录“已经报过警”。
     * @return 新事件已接收或已合入已有事件时返回 true；关闭上报、参数错误等拒绝情况返回 false。
     * 不需要传入参数。true 不保证图片/视频已生成或服务器已收到；需要细分时查看 status 和 detail。
     * 示例：
     * @code
     * EventRequest request;
     * request.event_type = "person_alarm";
     * EventReportResult result = report_event(ctx, request);
     * if (!result.accepted())
     * {
     *     std::string reason = result.detail; // 查看为什么没有接收
     * }
     * @endcode
     */
    bool accepted() const
    {
        return status == EventReportStatus::CREATED || status == EventReportStatus::MERGED ||
               status == EventReportStatus::CREATED_MEDIA_FAILED;
    }
};

/** 自定义事件接收器，用于你自己编写的测试或事件处理方式。
 * 普通通道逻辑直接使用 report_event() 即可；需要自己收集事件时，继承本类并实现 report()。
 * 接收器要一直存在，直到使用它的测试会话结束。
 */
class EventSink
{
  public:
    /** @brief 清理你自己创建的事件接收器；只在编写独立测试接收器时需要关心。
     * @return 没有返回值；不需要手动调用析构函数。
     * 没有输入参数。先结束使用它的测试会话，再释放接收器；普通通道逻辑使用 report_event() 即可。
     * 示例：
     * @code
     * {
     *     MyEventReceiver receiver; // 假设这是你自己实现的 EventSink 子类
     * } // 使用结束后自动清理
     * @endcode
     */
    virtual ~EventSink() = default;
    /** @brief 自己编写事件接收器时，实现这个函数来收到通道发出的报警。
     * @param context 发出事件的通道信息，例如 context.chnId 是通道 ID。
     * @param request 事件内容：类型、说明文字和附加字段。需要稍后处理时，请复制所需内容。
     * @return 你实现的函数要返回接收结果，例如 status=CREATED 表示接收，detail 可说明原因。
     * 这里的 sink 表示你创建的接收器。普通通道逻辑直接调用 report_event(ctx, request)，无需实现本类。
     * 示例：
     * @code
     * EventRequest request;
     * request.event_type = "person_alarm";
     * EventReportResult result = sink.report(*ctx, request);
     * @endcode
     */
    virtual EventReportResult report(ChannelContext &context, const EventRequest &request) = 0;
    /** @brief 让自定义事件接收器也能收到全局逻辑发出的报警，用于多通道逻辑测试。
     * @param context 发出事件的全局逻辑信息。
     * @param request 本次报警内容，需要稍后使用的数据应自行复制。
     * @return 默认返回 INVALID_REQUEST，表示不支持全局事件；自己实现此函数后由你返回接收结果。
     * 普通全局逻辑用 report_event(gctx, request)；只有自定义接收器时才需要处理这里。
     * 示例：
     * @code
     * EventRequest request;
     * request.event_type = "total_alarm";
     * EventReportResult result = sink.report(*gctx, request);
     * @endcode
     */
    virtual EventReportResult report(GlobalContext &context, const EventRequest &request);
};

/** @brief 把事件提交结果转成便于打印的英文文字。
 * @param status 事件结果中的 status，例如 result.status。
 * @return 返回 "created"、"disabled" 等文字；未知状态为 "unknown"，不要修改或释放这段文字。
 * 示例：
 * @code
 * const char *name = event_report_status_name(EventReportStatus::CREATED);
 * // name 是 "created"
 * @endcode
 */
const char *event_report_status_name(EventReportStatus status);

/** @brief 提交当前通道的一条报警，程序按上报配置处理记录、图片、视频和发送。
 * @param ctx 程序传给你的通道处理函数的 ctx。
 * @param request 报警内容；event_type 必须非空，并对应本模块 logic.json 的事件名称；message 和 fields
 * 按需填写。
 * @return 返回 EventReportResult：accepted() 判断是否接收，status 给出具体状态，detail 说明原因。
 * 请先配置上报功能；ctx 为空、事件名称为空或当前无法提交时会返回失败结果。
 * 通常只在刚触发报警或达到上报间隔时调用；想让截图带上画框和文字，应先 draw_* 再提交。
 * 示例：
 * @code
 * EventRequest request;
 * request.event_type = "person_alarm";
 * request.message = "发现人员进入";
 * request.fields = {event_field("count", 1)};
 * EventReportResult result = report_event(ctx, request);
 * if (!result.accepted())
 * {
 *     draw_text(ctx, result.detail.c_str(), {20, 40});
 * }
 * @endcode
 */
EventReportResult report_event(ChannelContext *ctx, const EventRequest &request);
/** @brief 提交多通道共同判断出的报警，例如多个摄像头一起发现异常。
 * @param ctx 程序传给全局逻辑的 gctx，不能传通道的 ChannelContext。
 * @param request 报警内容；event_type 必填，evidence_channel_ids 可填参与本次判断的通道 ID。
 * @return 返回提交结果；accepted() 判断是否接收，失败原因看 detail，具体状态看 status。
 * 最终使用哪些通道的图片仍由网页上的上报配置决定；只有明确来自某一路时才设置 source_channel_id。
 * 需要用到的画框和文字先设置好再提交；不要每次处理都无条件重复上报。
 * 示例：
 * @code
 * EventRequest request;
 * request.event_type = "total_alarm";
 * request.message = "多个通道发现异常";
 * request.evidence_channel_ids = {2, 3};
 * EventReportResult result = report_event(gctx, request);
 * @endcode
 */
EventReportResult report_event(GlobalContext *ctx, const EventRequest &request);
