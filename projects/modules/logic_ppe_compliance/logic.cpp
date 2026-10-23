#include <channel.h>
#include <drawing.h>
#include <events.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

enum ViolationType
{
    NO_HELMET = 0,
    NO_WORKWEAR,
    NO_GLOVES,
    VIOLATION_TYPE_COUNT
};

struct PersonEvidence
{
    std::array<std::deque<uint64_t>, VIOLATION_TYPE_COUNT> observations;
    std::array<bool, VIOLATION_TYPE_COUNT> confirmed{};
    uint64_t last_seen_ms = 0;
    bool reported = false;
};

struct PpeLogicState
{
    std::unordered_map<int, PersonEvidence> people;
};

struct TrackedPerson
{
    const AlgoResult *detection = nullptr;
    PersonEvidence *evidence = nullptr;
    std::array<bool, VIOLATION_TYPE_COUNT> observed{};
};

const char *violation_label(ViolationType type)
{
    switch (type)
    {
    case NO_HELMET:
        return "未戴安全帽";
    case NO_WORKWEAR:
        return "未穿工作服";
    case NO_GLOVES:
        return "未戴手套";
    case VIOLATION_TYPE_COUNT:
        break;
    }
    return "未知违规";
}

bool matches_label(const std::string &label, const std::string &configured_labels)
{
    size_t begin = 0;
    while (begin < configured_labels.size())
    {
        const size_t end = configured_labels.find(',', begin);
        const size_t length = (end == std::string::npos ? configured_labels.size() : end) - begin;
        size_t first = begin;
        size_t last = begin + length;
        while (first < last && configured_labels[first] == ' ')
            ++first;
        while (last > first && configured_labels[last - 1] == ' ')
            --last;
        if (label == configured_labels.substr(first, last - first))
            return true;
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return false;
}

int unique_person_for_box(const cv::Rect &box, const std::vector<TrackedPerson> &people)
{
    const cv::Point center(box.x + box.width / 2, box.y + box.height / 2);
    int match = -1;
    for (size_t index = 0; index < people.size(); ++index)
    {
        if (!people[index].detection->box.contains(center))
            continue;
        if (match >= 0)
            return -1;
        match = static_cast<int>(index);
    }
    return match;
}

static void logic_ppe_compliance(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results)
        return;
    if (!*ctx->state)
        *ctx->state = std::make_shared<PpeLogicState>();
    PpeLogicState &state = *std::static_pointer_cast<PpeLogicState>(*ctx->state);

    if (!ctx->infer_enabled)
    {
        ctx->publish_bool("ppe_alarm", false);
        ctx->publish_int("ppe_confirmed_people", 0);
        ctx->publish_int("ppe_violation_observations", 0);
        draw_text(ctx, "PPE穿戴检测: 推理已关闭", cv::Point(18, 32), cv::Scalar(180, 180, 180), 0.7, 1);
        return;
    }

    const std::string person_labels = ctx->param_string("person_labels");
    const std::array<std::string, VIOLATION_TYPE_COUNT> violation_labels = {
        ctx->param_string("no_helmet_labels"),
        ctx->param_string("no_workwear_labels"),
        ctx->param_string("no_gloves_labels")};
    const std::array<bool, VIOLATION_TYPE_COUNT> enabled = {
        ctx->param_bool("check_helmet"),
        ctx->param_bool("check_workwear"),
        ctx->param_bool("check_gloves")};
    const float min_person_score = static_cast<float>(ctx->param_float("person_min_score"));
    const float min_violation_score = static_cast<float>(ctx->param_float("violation_min_score"));
    const uint64_t evidence_window_ms = static_cast<uint64_t>(ctx->param_int("evidence_window_sec")) * 1000U;
    const uint64_t evidence_gap_ms = static_cast<uint64_t>(ctx->param_int("evidence_gap_ms"));
    const int required_observations = static_cast<int>(ctx->param_int("required_observations"));
    const uint64_t required_span_ms = static_cast<uint64_t>(ctx->param_int("required_span_ms"));
    const uint64_t now_ms = ctx->timestamp_ms;

    std::vector<TrackedPerson> people;
    for (const AlgoResult &result : *ctx->results)
    {
        if (result.track_id < 0 || result.score < min_person_score ||
            !matches_label(result.label, person_labels))
            continue;
        PersonEvidence &evidence = state.people[result.track_id];
        evidence.last_seen_ms = now_ms;
        people.push_back({&result, &evidence, {}});
    }

    for (const AlgoResult &result : *ctx->results)
    {
        if (result.score < min_violation_score)
            continue;
        for (int type = 0; type < VIOLATION_TYPE_COUNT; ++type)
        {
            if (!enabled[type] || !matches_label(result.label, violation_labels[type]))
                continue;
            const int person_index = unique_person_for_box(result.box, people);
            if (person_index >= 0 && people[person_index].detection->frame_id == result.frame_id)
                people[person_index].observed[type] = true;
        }
    }

    int observed_violations = 0;
    int confirmed_people = 0;
    for (TrackedPerson &person : people)
    {
        bool has_confirmed_violation = false;
        for (int type = 0; type < VIOLATION_TYPE_COUNT; ++type)
        {
            if (!enabled[type])
                continue;
            std::deque<uint64_t> &observations = person.evidence->observations[type];
            while (!observations.empty() && now_ms >= observations.front() &&
                   now_ms - observations.front() > evidence_window_ms)
                observations.pop_front();
            if (person.observed[type])
            {
                ++observed_violations;
                if (observations.empty() || now_ms < observations.back() ||
                    now_ms - observations.back() >= evidence_gap_ms)
                    observations.push_back(now_ms);
            }
            if (!person.evidence->confirmed[type] &&
                observations.size() >= static_cast<size_t>(required_observations) &&
                observations.back() - observations.front() >= required_span_ms)
                person.evidence->confirmed[type] = true;
            has_confirmed_violation = has_confirmed_violation || person.evidence->confirmed[type];
        }
        if (has_confirmed_violation)
        {
            ++confirmed_people;
            if (!person.evidence->reported)
            {
                std::string violations;
                for (int type = 0; type < VIOLATION_TYPE_COUNT; ++type)
                {
                    if (!person.evidence->confirmed[type])
                        continue;
                    if (!violations.empty())
                        violations += ", ";
                    violations += violation_label(static_cast<ViolationType>(type));
                }
                EventRequest request;
                request.event_type = "logic_ppe_violation";
                request.message = "人员安全穿戴违规: " + violations;
                request.fields = {
                    event_field("track_id", person.detection->track_id),
                    event_field("violation_types", violations),
                    event_field("person_score", person.detection->score)};
                if (report_event(ctx, request).accepted())
                    person.evidence->reported = true;
            }
        }
    }

    char status[192];
    std::snprintf(status, sizeof(status), "PPE违规候选: %d  已确认人员: %d", observed_violations, confirmed_people);
    draw_text(ctx, status, cv::Point(18, 32), confirmed_people ? cv::Scalar(0, 0, 255) : cv::Scalar(240, 240, 240),
              0.7, 1, DrawCommand::ALL, true);
    ctx->publish_bool("ppe_alarm", confirmed_people > 0);
    ctx->publish_int("ppe_confirmed_people", confirmed_people);
    ctx->publish_int("ppe_violation_observations", observed_violations);
}

} // namespace

REGISTER_LOGIC(logic_ppe_compliance);
