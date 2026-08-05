#include "logic/core/logic_common.h"

#include <cstdint>
#include <memory>
#include <sstream>

namespace
{

/* 本逻辑只使用模型类别编号，不依赖 labels.txt 中的标签名称。 */
constexpr int PERSON_CLASS_ID = 0;
constexpr int HELMET_CLASS_ID = 1;

const cv::Scalar GREEN(0, 200, 0);
const cv::Scalar RED(0, 0, 255);
const cv::Scalar ORANGE(0, 140, 255);
const cv::Scalar YELLOW(0, 255, 255);
const cv::Scalar WHITE(255, 255, 255);
const cv::Scalar BLACK(0, 0, 0);

struct ViolationLatch
{
    bool active = false;
    uint64_t clear_since_ms = 0;
};

struct HelmetState
{
    ViolationLatch no_helmet;
    ViolationLatch circle_intrusion;
};

HelmetState &helmet_state(ChannelContext *ctx)
{
    if (!*ctx->state)
        *ctx->state = std::make_shared<HelmetState>();
    return *std::static_pointer_cast<HelmetState>(*ctx->state);
}

bool latch_report_edge(ViolationLatch &latch, bool violation, uint64_t now_ms, uint64_t rearm_ms)
{
    if (violation)
    {
        latch.clear_since_ms = 0;
        if (!latch.active)
        {
            latch.active = true;
            return true;
        }
        return false;
    }

    if (!latch.active)
        return false;

    if (rearm_ms == 0)
    {
        latch.active = false;
        latch.clear_since_ms = 0;
        return false;
    }

    if (latch.clear_since_ms == 0)
        latch.clear_since_ms = now_ms;
    else if (now_ms >= latch.clear_since_ms && now_ms - latch.clear_since_ms >= rearm_ms)
    {
        latch.active = false;
        latch.clear_since_ms = 0;
    }
    return false;
}

cv::Rect helmet_match_region(const AlgoResult &person, float head_ratio, float margin_ratio)
{
    const int margin_x = static_cast<int>(person.box.width * margin_ratio + 0.5f);
    const int margin_y = static_cast<int>(person.box.height * margin_ratio + 0.5f);
    const int head_height = std::max(1, static_cast<int>(person.box.height * head_ratio + 0.5f));
    return cv::Rect(person.box.x - margin_x, person.box.y - margin_y, person.box.width + margin_x * 2,
                    head_height + margin_y);
}

struct MatchCandidate
{
    size_t person_index = 0;
    size_t helmet_index = 0;
    int distance_sq = 0;
};

std::vector<bool> match_helmets(const std::vector<AlgoResult *> &persons, const std::vector<AlgoResult *> &helmets,
                                float head_ratio, float margin_ratio)
{
    std::vector<MatchCandidate> candidates;
    for (size_t person_index = 0; person_index < persons.size(); ++person_index)
    {
        const cv::Rect region = helmet_match_region(*persons[person_index], head_ratio, margin_ratio);
        const cv::Point person_head(persons[person_index]->box.x + persons[person_index]->box.width / 2,
                                    persons[person_index]->box.y);
        for (size_t helmet_index = 0; helmet_index < helmets.size(); ++helmet_index)
        {
            const cv::Point helmet_center = helmets[helmet_index]->box_center();
            if (!region.contains(helmet_center))
                continue;
            const int dx = helmet_center.x - person_head.x;
            const int dy = helmet_center.y - person_head.y;
            candidates.push_back({person_index, helmet_index, dx * dx + dy * dy});
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const MatchCandidate &left, const MatchCandidate &right) {
        return left.distance_sq < right.distance_sq;
    });

    std::vector<bool> person_matched(persons.size(), false);
    std::vector<bool> helmet_matched(helmets.size(), false);
    for (const MatchCandidate &candidate : candidates)
    {
        if (person_matched[candidate.person_index] || helmet_matched[candidate.helmet_index])
            continue;
        person_matched[candidate.person_index] = true;
        helmet_matched[candidate.helmet_index] = true;
    }
    return person_matched;
}

std::string track_ids_json(const std::vector<AlgoResult *> &targets)
{
    std::ostringstream output;
    output << '[';
    bool first = true;
    for (const AlgoResult *target : targets)
    {
        if (!target || target->track_id < 0)
            continue;
        if (!first)
            output << ',';
        output << target->track_id;
        first = false;
    }
    output << ']';
    return output.str();
}

void report_failure(const ChannelContext *ctx, const char *event_type, const EventReportResult &report)
{
    if (report.accepted())
        return;
    std::fprintf(stderr, "[logic_helmet][ch%02d] %s report rejected: status=%s detail=%s\n", ctx->chnId,
                 event_type, event_report_status_name(report.status), report.detail.c_str());
}

} // namespace

static void logic_helmet(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->frame || ctx->frame->empty() || !ctx->results)
        return;

    HelmetState &state = helmet_state(ctx);
    const std::string roi_name = ctx->param_string("roi_name");
    const int circle_radius = std::max(1, static_cast<int>(ctx->param_int("circle_radius")));
    const float person_min_score = std::max(0.0f, ctx->param_float("person_min_score"));
    const float helmet_min_score = std::max(0.0f, ctx->param_float("helmet_min_score"));
    const float head_ratio = std::max(0.1f, std::min(1.0f, ctx->param_float("head_region_ratio")));
    const float margin_ratio = std::max(0.0f, std::min(0.5f, ctx->param_float("helmet_match_margin_ratio")));
    const float rearm_seconds = std::max(0.0f, ctx->param_float("rearm_seconds"));
    const uint64_t rearm_ms = static_cast<uint64_t>(rearm_seconds * 1000.0f);

    const cv::Point frame_center(ctx->frame->cols / 2, ctx->frame->rows / 2);
    const int64_t radius_sq = static_cast<int64_t>(circle_radius) * circle_radius;

    std::vector<AlgoResult *> roi_persons;
    std::vector<AlgoResult *> helmets;
    std::vector<AlgoResult *> circle_persons;
    bool roi_available = ctx->rois && !ctx->rois->empty();
    int selected_roi_index = ROI_ALL;
    if (roi_available && !roi_name.empty())
    {
        selected_roi_index = roi_find(ctx, roi_name.c_str());
        roi_available = selected_roi_index >= 0;
    }

    for (AlgoResult &result : *ctx->results)
    {
        if (result.class_id == HELMET_CLASS_ID && result.score >= helmet_min_score)
        {
            helmets.push_back(&result);
            continue;
        }
        if (result.class_id != PERSON_CLASS_ID || result.score < person_min_score)
            continue;

        if (roi_available && roi_contains(ctx, result.box, selected_roi_index))
            roi_persons.push_back(&result);

        const cv::Point center = result.box_center();
        const int64_t dx = static_cast<int64_t>(center.x) - frame_center.x;
        const int64_t dy = static_cast<int64_t>(center.y) - frame_center.y;
        if (dx * dx + dy * dy <= radius_sq)
            circle_persons.push_back(&result);
    }

    const std::vector<bool> person_has_helmet = match_helmets(roi_persons, helmets, head_ratio, margin_ratio);
    std::vector<AlgoResult *> unhelmeted_persons;
    for (size_t index = 0; index < roi_persons.size(); ++index)
    {
        if (person_has_helmet[index])
            roi_persons[index]->box_color = GREEN;
        else
        {
            roi_persons[index]->box_color = RED;
            unhelmeted_persons.push_back(roi_persons[index]);
        }
    }
    for (AlgoResult *person : circle_persons)
    {
        if (person->box_color != RED)
            person->box_color = ORANGE;
    }

    const bool no_helmet_violation = roi_available && !unhelmeted_persons.empty();
    const bool circle_violation = !circle_persons.empty();
    const bool report_no_helmet = latch_report_edge(state.no_helmet, no_helmet_violation, ctx->timestamp_ms, rearm_ms);
    const bool report_circle =
        latch_report_edge(state.circle_intrusion, circle_violation, ctx->timestamp_ms, rearm_ms);

    draw_circle(ctx, frame_center, circle_radius, circle_violation ? RED : GREEN, circle_violation ? 4 : 2);
    draw_circle(ctx, frame_center, 4, circle_violation ? RED : YELLOW, -1);

    char line[256];
    if (!roi_available)
    {
        const char *warning = roi_name.empty() ? "未配置有效ROI，未佩戴安全帽检测暂停"
                                               : "未找到指定ROI，未佩戴安全帽检测暂停";
        draw_text(ctx, warning, cv::Point(16, 32), BLACK, 0.72, 4);
        draw_text(ctx, warning, cv::Point(16, 32), YELLOW, 0.72, 1);
    }
    else
    {
        std::snprintf(line, sizeof(line), "ROI人员: %zu  未佩戴安全帽: %zu", roi_persons.size(),
                      unhelmeted_persons.size());
        draw_text(ctx, line, cv::Point(16, 32), BLACK, 0.72, 4);
        draw_text(ctx, line, cv::Point(16, 32), no_helmet_violation ? RED : WHITE, 0.72, 1);
    }

    std::snprintf(line, sizeof(line), "中心圆人员: %zu  半径: %dpx", circle_persons.size(), circle_radius);
    draw_text(ctx, line, cv::Point(16, 66), BLACK, 0.72, 4);
    draw_text(ctx, line, cv::Point(16, 66), circle_violation ? RED : WHITE, 0.72, 1);

    /* 绘制必须先于 report_event，确保事件图片和视频带有本帧违规标注。 */
    if (report_no_helmet)
    {
        EventRequest request;
        request.event_type = "helmet_missing";
        request.message = "ROI区域内检测到未佩戴安全帽的人员";
        request.merge_mode = EventMergeMode::NEVER;
        request.fields.set_number("person_class_id", PERSON_CLASS_ID);
        request.fields.set_number("helmet_class_id", HELMET_CLASS_ID);
        request.fields.set_string("roi_name", roi_name.empty() ? "ALL" : roi_name);
        request.fields.set_number("roi_person_count", roi_persons.size());
        request.fields.set_number("unhelmeted_person_count", unhelmeted_persons.size());
        request.fields.set_json("unhelmeted_track_ids", track_ids_json(unhelmeted_persons));
        report_failure(ctx, request.event_type.c_str(), report_event(ctx, request));
    }

    if (report_circle)
    {
        EventRequest request;
        request.event_type = "center_circle_person";
        request.message = "画面中心圆形区域内检测到人员";
        request.merge_mode = EventMergeMode::NEVER;
        request.fields.set_number("person_class_id", PERSON_CLASS_ID);
        request.fields.set_number("circle_center_x", frame_center.x);
        request.fields.set_number("circle_center_y", frame_center.y);
        request.fields.set_number("circle_radius", circle_radius);
        request.fields.set_number("circle_person_count", circle_persons.size());
        request.fields.set_json("circle_person_track_ids", track_ids_json(circle_persons));
        report_failure(ctx, request.event_type.c_str(), report_event(ctx, request));
    }
}

REGISTER_LOGIC(logic_helmet);
