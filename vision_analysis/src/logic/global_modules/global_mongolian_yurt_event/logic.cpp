#include "inference/inference_engine.h"
#include "logic/core/global_logic.h"
#include "cJSON.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{
struct SeenSequences
{
    int64_t water = 0;
    int64_t fall = 0;
    int64_t help = 0;
};

struct Candidate
{
    std::string type;
    int channel_id = -1;
    std::string hardware_id;
    int64_t seq = 0;
    uint64_t unix_ms = 0;
    int64_t track_id = -1;
    double quality = 0.0;
    double view_scraper_mask_ratio = 0.0;
    double person_scraper_overlap_ratio = 0.0;
};

struct RadarState
{
    bool received = false;
    std::string sensor_id;
    std::string coordinate_frame;
    uint64_t captured_unix_ms = 0;
    uint64_t received_steady_ms = 0;
    int target_count = 0;
    std::string targets_json = "[]";
};

struct FusionState
{
    std::map<int, SeenSequences> seen;
    std::vector<Candidate> pending;
    std::vector<Candidate> ready;
    uint64_t last_incident_activity_ms = 0;
    uint64_t last_report_attempt_ms = 0;
    RadarState radar;
};

FusionState &fusion_state(GlobalContext *gctx)
{
    if (!*gctx->state)
        *gctx->state = std::make_shared<FusionState>();
    return *std::static_pointer_cast<FusionState>(*gctx->state);
}

std::string print_json(cJSON *item)
{
    char *text = item ? cJSON_PrintUnformatted(item) : nullptr;
    const std::string result = text ? text : "null";
    if (text)
        cJSON_free(text);
    return result;
}

int priority(const Candidate &candidate)
{
    if (candidate.type == "water_entry")
        return 0;
    if (candidate.type == "person_fall")
        return 1;
    return 2;
}

const char *event_message(const std::string &type)
{
    if (type == "water_entry")
        return "蒙古包污水池人员落水事件";
    if (type == "person_fall")
        return "蒙古包外围走廊人员跌倒事件";
    return "蒙古包场景人员挥手求救事件";
}

bool candidate_better(const Candidate &left, const Candidate &right)
{
    if (priority(left) != priority(right))
        return priority(left) < priority(right);
    if (left.quality != right.quality)
        return left.quality > right.quality;
    if (left.view_scraper_mask_ratio != right.view_scraper_mask_ratio)
        return left.view_scraper_mask_ratio < right.view_scraper_mask_ratio;
    if (left.unix_ms != right.unix_ms)
        return left.unix_ms < right.unix_ms;
    return left.channel_id < right.channel_id;
}

void ingest_candidate(FusionState &state, const ChannelInput &input, const char *kind, const char *seq_key,
                      const char *time_key, const char *track_key, const char *quality_key,
                      int64_t &last_seq, uint64_t now_unix_ms)
{
    int64_t seq = 0;
    int64_t event_time = 0;
    if (!input.read_int(seq_key, &seq))
        return;
    if (seq <= 0)
    {
        /* 通道 Logic 重启后序号会归零；先观察到零，下一次 seq=1 才能被当作新事件。 */
        last_seq = 0;
        return;
    }
    if (seq < last_seq)
        last_seq = 0;
    if (seq == last_seq)
        return;
    last_seq = seq;
    if (!input.read_int(time_key, &event_time) || event_time <= 0)
        return;
    const uint64_t unix_ms = static_cast<uint64_t>(event_time);
    if (now_unix_ms > unix_ms && now_unix_ms - unix_ms > 30000)
        return;

    Candidate candidate;
    candidate.type = kind;
    candidate.channel_id = input.channel_id();
    candidate.hardware_id = input.get_string("hardware_id");
    candidate.seq = seq;
    candidate.unix_ms = unix_ms;
    candidate.track_id = input.get_int(track_key, -1);
    candidate.quality = input.get_number(quality_key);
    candidate.view_scraper_mask_ratio = input.get_number("view_scraper_mask_ratio");
    if (candidate.type == "water_entry")
        candidate.person_scraper_overlap_ratio = input.get_number("water_person_scraper_overlap_ratio");
    state.pending.push_back(candidate);
}

uint64_t newest_time(const std::vector<Candidate> &candidates)
{
    uint64_t result = 0;
    for (const Candidate &candidate : candidates)
        result = std::max(result, candidate.unix_ms);
    return result;
}

std::string candidates_json(const std::vector<Candidate> &candidates)
{
    cJSON *array = cJSON_CreateArray();
    for (const Candidate &candidate : candidates)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", candidate.type.c_str());
        cJSON_AddNumberToObject(item, "channelId", candidate.channel_id);
        cJSON_AddStringToObject(item, "hardwareId", candidate.hardware_id.c_str());
        cJSON_AddNumberToObject(item, "trackId", static_cast<double>(candidate.track_id));
        cJSON_AddNumberToObject(item, "occurredAtMs", static_cast<double>(candidate.unix_ms));
        cJSON_AddNumberToObject(item, "quality", candidate.quality);
        cJSON_AddNumberToObject(item, "viewScraperMaskRatio", candidate.view_scraper_mask_ratio);
        cJSON_AddNumberToObject(item, "personScraperOverlapRatio", candidate.person_scraper_overlap_ratio);
        cJSON_AddItemToArray(array, item);
    }
    const std::string result = print_json(array);
    cJSON_Delete(array);
    return result;
}

std::string incident_types_json(const std::vector<Candidate> &candidates)
{
    std::set<std::string> values;
    for (const Candidate &candidate : candidates)
        values.insert(candidate.type);
    cJSON *array = cJSON_CreateArray();
    for (const std::string &value : values)
        cJSON_AddItemToArray(array, cJSON_CreateString(value.c_str()));
    const std::string result = print_json(array);
    cJSON_Delete(array);
    return result;
}

std::string channel_ids_json(const std::vector<Candidate> &candidates)
{
    std::set<int> values;
    for (const Candidate &candidate : candidates)
        values.insert(candidate.channel_id);
    cJSON *array = cJSON_CreateArray();
    for (int value : values)
        cJSON_AddItemToArray(array, cJSON_CreateNumber(value));
    const std::string result = print_json(array);
    cJSON_Delete(array);
    return result;
}

bool parse_radar_payload(const LogicAction *action, RadarState *radar, std::string *error)
{
    cJSON *root = cJSON_Parse(action->payload_json.c_str());
    if (!cJSON_IsObject(root))
    {
        if (error)
            *error = "payload must be a JSON object";
        cJSON_Delete(root);
        return false;
    }
    cJSON *targets = cJSON_GetObjectItemCaseSensitive(root, "targets");
    if (!cJSON_IsArray(targets))
    {
        if (error)
            *error = "targets must be an array";
        cJSON_Delete(root);
        return false;
    }
    cJSON *target = nullptr;
    cJSON_ArrayForEach(target, targets)
    {
        cJSON *x = cJSON_GetObjectItemCaseSensitive(target, "x");
        cJSON *y = cJSON_GetObjectItemCaseSensitive(target, "y");
        cJSON *z = cJSON_GetObjectItemCaseSensitive(target, "z");
        if (!cJSON_IsObject(target) || !cJSON_IsNumber(x) || !cJSON_IsNumber(y) || !cJSON_IsNumber(z))
        {
            if (error)
                *error = "each target requires numeric x, y and z";
            cJSON_Delete(root);
            return false;
        }
    }

    cJSON *sensor = cJSON_GetObjectItemCaseSensitive(root, "sensor_id");
    cJSON *frame = cJSON_GetObjectItemCaseSensitive(root, "coordinate_frame");
    cJSON *captured = cJSON_GetObjectItemCaseSensitive(root, "captured_unix_ms");
    radar->received = true;
    radar->sensor_id = cJSON_IsString(sensor) && sensor->valuestring ? sensor->valuestring : "";
    radar->coordinate_frame = cJSON_IsString(frame) && frame->valuestring ? frame->valuestring : "";
    radar->captured_unix_ms = cJSON_IsNumber(captured) && captured->valuedouble > 0.0
                                   ? static_cast<uint64_t>(captured->valuedouble)
                                   : action->received_unix_ms;
    radar->target_count = cJSON_GetArraySize(targets);
    radar->targets_json = print_json(targets);
    cJSON_Delete(root);
    return true;
}
} // namespace

static LogicActionResult global_mongolian_yurt_event_action(GlobalContext *gctx, const LogicAction *action)
{
    LogicActionResult result;
    if (!gctx || !gctx->state || !action)
        return result;
    FusionState &state = fusion_state(gctx);
    if (action->name == "radar_clear")
    {
        state.radar = RadarState{};
        result.handled = true;
        result.message = "radar state cleared";
        return result;
    }
    if (action->name != "radar_update")
    {
        result.message = "unknown action";
        return result;
    }
    std::string error;
    if (!parse_radar_payload(action, &state.radar, &error))
    {
        result.handled = true;
        result.message = error;
        return result;
    }
    state.radar.received_steady_ms = gctx->timestamp_ms;
    result.handled = true;
    result.message = "radar frame accepted";
    return result;
}

static void global_mongolian_yurt_event(GlobalContext *gctx)
{
    if (!gctx || !gctx->state)
        return;
    FusionState &state = fusion_state(gctx);
    const uint64_t fusion_ms = static_cast<uint64_t>(gctx->param_float("fusion_window_seconds") * 1000.0F);
    const uint64_t dedup_ms = static_cast<uint64_t>(gctx->param_float("same_scene_dedup_seconds") * 1000.0F);
    const uint64_t retry_ms = static_cast<uint64_t>(gctx->param_float("report_retry_seconds") * 1000.0F);

    for (const ChannelInput &input : gctx->inputs())
    {
        if (input.logic_name() != "logic_fall_detection")
            continue;
        SeenSequences &seen = state.seen[input.channel_id()];
        ingest_candidate(state, input, "water_entry", "water_candidate_seq", "water_candidate_unix_ms",
                         "water_candidate_track_id", "water_evidence_quality", seen.water, gctx->unix_ms);
        ingest_candidate(state, input, "person_fall", "fall_candidate_seq", "fall_candidate_unix_ms",
                         "fall_candidate_track_id", "fall_evidence_quality", seen.fall, gctx->unix_ms);
        ingest_candidate(state, input, "help_gesture", "help_candidate_seq", "help_candidate_unix_ms",
                         "help_candidate_track_id", "help_evidence_quality", seen.help, gctx->unix_ms);
    }

    if (state.ready.empty() && !state.pending.empty())
    {
        std::sort(state.pending.begin(), state.pending.end(),
                  [](const Candidate &a, const Candidate &b) { return a.unix_ms < b.unix_ms; });
        const uint64_t start = state.pending.front().unix_ms;
        if (gctx->unix_ms >= start && gctx->unix_ms - start >= fusion_ms)
        {
            auto end = std::upper_bound(state.pending.begin(), state.pending.end(), start + fusion_ms,
                                        [](uint64_t time, const Candidate &candidate) {
                                            return time < candidate.unix_ms;
                                        });
            state.ready.assign(state.pending.begin(), end);
            state.pending.erase(state.pending.begin(), end);

            // 与原 Python 融合器一致：同一相机同一类型在一个融合窗内只保留最佳证据。
            std::map<std::pair<int, std::string>, Candidate> best_by_channel_and_type;
            for (const Candidate &candidate : state.ready)
            {
                const auto key = std::make_pair(candidate.channel_id, candidate.type);
                auto found = best_by_channel_and_type.find(key);
                if (found == best_by_channel_and_type.end() || candidate.quality > found->second.quality)
                    best_by_channel_and_type[key] = candidate;
            }
            state.ready.clear();
            for (const auto &entry : best_by_channel_and_type)
                state.ready.push_back(entry.second);

            const uint64_t latest = newest_time(state.ready);
            if (state.last_incident_activity_ms > 0 &&
                latest <= state.last_incident_activity_ms + dedup_ms)
            {
                state.last_incident_activity_ms = std::max(state.last_incident_activity_ms, latest);
                state.ready.clear();
            }
        }
    }

    if (state.ready.empty() || (state.last_report_attempt_ms > 0 &&
                                gctx->timestamp_ms - state.last_report_attempt_ms < retry_ms))
        return;

    const Candidate &primary = *std::min_element(state.ready.begin(), state.ready.end(), candidate_better);
    const bool radar_available = gctx->param_bool("radar_enabled") && state.radar.received &&
                                 gctx->timestamp_ms >= state.radar.received_steady_ms &&
                                 gctx->timestamp_ms - state.radar.received_steady_ms <=
                                     static_cast<uint64_t>(gctx->param_int("radar_freshness_ms"));
    const std::string types_json = incident_types_json(state.ready);
    const std::string channels_json = channel_ids_json(state.ready);
    const std::string candidates_value = candidates_json(state.ready);
    const int64_t radar_age_ms = radar_available
                                     ? static_cast<int64_t>(gctx->timestamp_ms - state.radar.received_steady_ms)
                                     : -1;

    EventRequest request;
    request.event_type = "mongolian_yurt_event";
    request.message = event_message(primary.type);
    request.source_channel_id = primary.channel_id;
    request.merge_mode = EventMergeMode::NEVER;
    request.fields = {
        event_field("hardware_id", primary.hardware_id.empty() ? gctx->param_string("default_hardware_id")
                                                               : primary.hardware_id),
        event_field("platform_source", gctx->param_string("platform_source")),
        event_field("platform_event_type", gctx->param_string("platform_event_type")),
        /* 兼容既有 objectInvadeDet 协议；融合与雷达明细保留在其它事件字段中。 */
        event_json_field("det_result", "{}"),
        event_field("invade_flag", gctx->param_int("invade_flag")),
        event_field("primary_event_type", primary.type),
        event_field("primary_channel_id", primary.channel_id),
        event_field("primary_track_id", primary.track_id),
        event_field("candidate_count", static_cast<int64_t>(state.ready.size())),
        event_json_field("incident_types", types_json),
        event_json_field("candidate_channels", channels_json),
        event_json_field("candidates", candidates_value),
        event_field("evidence_quality", primary.quality),
        event_field("view_scraper_mask_ratio", primary.view_scraper_mask_ratio),
        event_field("person_scraper_overlap_ratio", primary.person_scraper_overlap_ratio),
        event_field("radar_available", radar_available),
        event_field("radar_sensor_id", radar_available ? state.radar.sensor_id : ""),
        event_field("radar_coordinate_frame", radar_available ? state.radar.coordinate_frame : ""),
        event_field("radar_target_count", radar_available ? state.radar.target_count : 0),
        event_field("radar_age_ms", radar_age_ms),
        event_json_field("radar_targets", radar_available ? state.radar.targets_json : "[]"),
    };

    state.last_report_attempt_ms = gctx->timestamp_ms;
    const EventReportResult report = report_event(gctx, request);
    if (report.accepted())
    {
        state.last_incident_activity_ms = newest_time(state.ready);
        state.ready.clear();
    }
    else
        std::fprintf(stderr, "[global_mongolian_yurt_event] report rejected: %s (%s)\n",
                     event_report_status_name(report.status), report.detail.c_str());
}

REGISTER_GLOBAL_LOGIC(global_mongolian_yurt_event);
REGISTER_GLOBAL_LOGIC_ACTION(global_mongolian_yurt_event, global_mongolian_yurt_event_action);
