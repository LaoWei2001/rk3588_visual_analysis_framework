/**
 * @file logic.cpp
 * @brief Dify 周期截图上报教学示例。
 *
 * 这个模块只负责三件事：
 *   1. 按 Web 画布配置的时间间隔触发事件；
 *   2. 把画布提示词和业务变量写入 EventRequest.fields；
 *   3. 在当前有效业务帧上调用 report_event()。
 *
 * 它不会直接访问 Dify，不读取 API Key，不上传文件，也不编码图片。图片生成、文件上传、
 * Dify inputs 字段映射和失败重试都由“上报配置”节点及 service/upload 统一完成。
 *
 * 配套文件：
 *   - logic.json：Web 参数、事件类型和上报字段声明；
 *   - README.md：画布接线、Dify 开始节点和二次开发说明；
 *   - report_templates/dify_periodic_snapshot.json：随模块打包的默认字段映射。
 */

#include "logic/core/logic_common.h"
#include "cJSON.h"

#include <cstdint>
#include <memory>

namespace
{

struct DifyPeriodicState
{
    bool initialized = false;
    uint64_t last_attempt_ms = 0;
    uint64_t accepted_sequence = 0;
    uint64_t last_error_log_ms = 0;
};

/**
 * 把本帧检测结果转换为一个合法 JSON 数组。
 *
 * 这里专门演示 event_json_field() 的用法。真实项目可以按业务需要增删字段，或直接把
 * 自己算法已经生成的 JSON 字符串传给 event_json_field()。max_items 用于避免目标很多时
 * 事件文件和 Dify inputs 过大；object_count 仍然保留本帧的完整目标数量。
 */
static std::string detections_to_json(const std::vector<AlgoResult> *results, int64_t max_items)
{
    cJSON *array = cJSON_CreateArray();
    if (!array)
        return "[]";

    if (results)
    {
        const size_t count = std::min(results->size(), static_cast<size_t>(std::max<int64_t>(0, max_items)));
        for (size_t index = 0; index < count; ++index)
        {
            const AlgoResult &result = (*results)[index];
            cJSON *item = cJSON_CreateObject();
            cJSON *box = cJSON_CreateObject();
            if (!item || !box)
            {
                cJSON_Delete(item);
                cJSON_Delete(box);
                break;
            }

            cJSON_AddStringToObject(item, "label", result.label.c_str());
            cJSON_AddNumberToObject(item, "score", result.score);
            cJSON_AddNumberToObject(item, "class_id", result.class_id);
            cJSON_AddNumberToObject(item, "track_id", result.track_id);
            cJSON_AddNumberToObject(box, "x", result.box.x);
            cJSON_AddNumberToObject(box, "y", result.box.y);
            cJSON_AddNumberToObject(box, "width", result.box.width);
            cJSON_AddNumberToObject(box, "height", result.box.height);
            cJSON_AddItemToObject(item, "box", box);
            cJSON_AddItemToArray(array, item);
        }
    }

    char *text = cJSON_PrintUnformatted(array);
    std::string json = text ? text : "[]";
    if (text)
        cJSON_free(text);
    cJSON_Delete(array);
    return json;
}

static void log_report_failure_periodically(ChannelContext *ctx, DifyPeriodicState &state,
                                            const EventReportResult &report)
{
    /* 未接上报节点时也只按正常周期尝试；错误日志最多每分钟一条，避免逐帧刷屏。 */
    if (state.last_error_log_ms != 0 && ctx->timestamp_ms - state.last_error_log_ms < 60000)
        return;
    state.last_error_log_ms = ctx->timestamp_ms;
    fprintf(stderr, "[logic_dify][ch%02d] event not created: %s (%s)\n", ctx->chnId,
            event_report_status_name(report.status), report.detail.c_str());
}

static void logic_dify(ChannelContext *ctx)
{
    /* 到达上报周期后，report_event() 才会通过 model_frame() 惰性取得截图。 */
    if (!ctx || !ctx->state)
        return;

    if (!*ctx->state)
        *ctx->state = std::make_shared<DifyPeriodicState>();
    DifyPeriodicState &state = *std::static_pointer_cast<DifyPeriodicState>(*ctx->state);

    const int64_t interval_sec = ctx->param_int("report_interval_sec");
    const uint64_t interval_ms = static_cast<uint64_t>(interval_sec) * 1000ULL;

    if (!state.initialized)
    {
        state.initialized = true;
        state.last_attempt_ms = ctx->timestamp_ms;

        /* 默认等待完整间隔；教学或联调时可以在画布开启“首帧立即上报”。 */
        if (!ctx->param_bool("first_report_immediately"))
            return;
    }
    else if (ctx->timestamp_ms - state.last_attempt_ms < interval_ms)
    {
        return;
    }

    /* 无论本次是否成功建档，都从本次尝试重新计时，避免配置错误时逐帧重复提交。 */
    state.last_attempt_ms = ctx->timestamp_ms;

    const uint64_t next_sequence = state.accepted_sequence + 1;
    const size_t object_count = ctx->results ? ctx->results->size() : 0;
    const bool has_objects = object_count > 0;
    const int64_t max_detections = ctx->param_int("max_detections");
    const bool detections_truncated = object_count > static_cast<size_t>(max_detections);

    EventRequest request;
    request.event_type = "dify_periodic_snapshot";
    request.message = "Dify 周期截图";
    request.merge_mode = EventMergeMode::NEVER;

    /*
     * 动态字段的完整写法示例：
     *   - prompt/scene_name/custom_text 来自 Web 画布 logic 参数；
     *   - object_count/has_objects/report_sequence 来自当前运行状态；
     *   - detections/custom_payload 是 JSON 对象或数组；
     *   - 通道 ID、抓拍时间等标准字段不必在这里复制，契约可直接使用
     *     source.channel_id、event.snap_time。
     *
     * 每个 key 都要同时声明在 logic.json.report_fields 中，Web 契约编辑器才会列出它。
     */
    request.fields = {
        event_field("prompt", ctx->param_string("prompt")),
        event_field("scene_name", ctx->param_string("scene_name")),
        event_field("custom_text", ctx->param_string("custom_text")),
        event_field("report_interval_sec", interval_sec),
        event_field("report_sequence", next_sequence),
        event_field("object_count", object_count),
        event_field("has_objects", has_objects),
        event_field("detections_truncated", detections_truncated),
        event_json_field("detections", detections_to_json(ctx->results, max_detections)),
        event_json_field("custom_payload", ctx->param_json("custom_payload")),
    };

    const EventReportResult report = report_event(ctx, request);
    if (report.accepted())
    {
        state.accepted_sequence = next_sequence;
        printf("[logic_dify][ch%02d] local event queued: %s, sequence=%llu\n", ctx->chnId,
               report.event_id.c_str(), static_cast<unsigned long long>(next_sequence));
    }
    else
    {
        log_report_failure_periodically(ctx, state, report);
    }
}

} // namespace

REGISTER_LOGIC(logic_dify);
