/*
 * 安全穿戴编排（通道适配层）：读参数 → ROI 过滤 → ppe_guard::Guard 状态机 → 绘图 / 上报 / 发布。
 *
 * 判定逻辑全部在 ppe_guard.h/.cpp 里，不接触 ChannelContext，便于 engine/tests/ppe 回归。
 * 本文件只负责：参数解析、作业区域过滤、画框、事件提交与输出发布。
 */
#include <channel.h>
#include <drawing.h>
#include <events.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ppe_guard.h"

namespace
{

struct GuardState
{
    ppe_guard::Guard guard;
    int unmatched_frames = 0;    /* 有检测结果却没有一个人命中 person_labels 的连续帧数 */
    float score_override = 0.0f; /* 现场按钮调过的灵敏度；0 表示跟随配置 */
    bool debug_visible = true;   /* 画面左上角调试信息是否显示 */
};

void publish_idle(ChannelContext *ctx)
{
    ctx->publish_bool("ppe_alarm", false);
    ctx->publish_int("confirmed_people", 0);
    ctx->publish_int("candidate_observations", 0);
    ctx->publish_int("rejected_observations", 0);
    ctx->publish_int("duplicate_observations", 0);
    ctx->publish_int("vetoed_observations", 0);
    ctx->publish_int("tracked_people", 0);
}

ppe_guard::Config load_config(ChannelContext *ctx)
{
    ppe_guard::Config config;
    const std::string glove_labels = ctx->param_string("no_gloves_labels");
    config.person_labels = ppe_guard::split_labels(ctx->param_string("person_labels"));
    config.violation_labels[ppe_guard::PART_HELMET] = ppe_guard::split_labels(ctx->param_string("no_helmet_labels"));
    config.violation_labels[ppe_guard::PART_WORKWEAR] =
        ppe_guard::split_labels(ctx->param_string("no_workwear_labels"));
    config.violation_labels[ppe_guard::PART_HAND_LEFT] = ppe_guard::split_labels(glove_labels);
    config.violation_labels[ppe_guard::PART_HAND_RIGHT] = ppe_guard::split_labels(glove_labels);

    const bool gloves_enabled = ctx->param_bool("check_gloves");
    config.enabled = {
        {ctx->param_bool("check_helmet"), ctx->param_bool("check_workwear"), gloves_enabled, gloves_enabled}};

    config.person_min_score = static_cast<float>(ctx->param_float("person_min_score"));
    config.violation_min_score = static_cast<float>(ctx->param_float("violation_min_score"));
    config.glove_min_score = static_cast<float>(ctx->param_float("glove_min_score"));
    config.keypoint_min_score = static_cast<float>(ctx->param_float("keypoint_min_score"));
    config.evidence_window_ms = static_cast<uint64_t>(ctx->param_int("evidence_window_sec")) * 1000U;
    config.evidence_gap_ms = static_cast<uint64_t>(ctx->param_int("evidence_gap_ms"));
    config.required_observations = static_cast<size_t>(ctx->param_int("required_observations"));
    config.required_span_ms = static_cast<uint64_t>(ctx->param_int("required_span_ms"));
    config.reacquire_gap_ms = static_cast<uint64_t>(ctx->param_int("reacquire_gap_ms"));
    config.state_ttl_ms = static_cast<uint64_t>(ctx->param_int("state_ttl_sec")) * 1000U;

    config.geometry_gate = ctx->param_bool("geometry_gate");
    config.helmet_top_ratio = static_cast<float>(ctx->param_float("helmet_top_ratio"));
    config.helmet_area_min_ratio = static_cast<float>(ctx->param_float("helmet_area_min_ratio"));
    config.helmet_area_max_ratio = static_cast<float>(ctx->param_float("helmet_area_max_ratio"));
    config.helmet_head_ratio = static_cast<float>(ctx->param_float("helmet_head_ratio"));
    config.helmet_search_up_ratio = static_cast<float>(ctx->param_float("helmet_search_up_ratio"));
    config.helmet_search_side_ratio = static_cast<float>(ctx->param_float("helmet_search_side_ratio"));
    config.workwear_top_ratio = static_cast<float>(ctx->param_float("workwear_top_ratio"));
    config.workwear_bottom_ratio = static_cast<float>(ctx->param_float("workwear_bottom_ratio"));
    config.glove_wrist_ratio = static_cast<float>(ctx->param_float("glove_wrist_ratio"));
    config.hand_attach_ratio = static_cast<float>(ctx->param_float("hand_attach_ratio"));
    config.dedupe_iou = static_cast<float>(ctx->param_float("dedupe_iou"));
    config.require_pose_for_violation = ctx->param_bool("require_pose_for_violation");
    config.positive_veto = ctx->param_bool("positive_veto");
    config.helmet_infer_by_absence = ctx->param_bool("helmet_infer_by_absence");
    config.workwear_require_positive = ctx->param_bool("workwear_require_positive");
    config.workwear_min_person_height = static_cast<float>(ctx->param_float("workwear_min_person_height"));
    config.absence_min_person_score = static_cast<float>(ctx->param_float("absence_min_person_score"));
    config.positive_min_score = static_cast<float>(ctx->param_float("positive_min_score"));
    config.glove_require_pose = ctx->param_bool("glove_require_pose");
    config.positive_labels[ppe_guard::PART_HELMET] =
        ppe_guard::split_labels(ctx->param_string("helmet_positive_labels"));
    config.positive_labels[ppe_guard::PART_WORKWEAR] =
        ppe_guard::split_labels(ctx->param_string("workwear_positive_labels"));
    const std::vector<std::string> glove_positive = ppe_guard::split_labels(ctx->param_string("glove_positive_labels"));
    config.positive_labels[ppe_guard::PART_HAND_LEFT] = glove_positive;
    config.positive_labels[ppe_guard::PART_HAND_RIGHT] = glove_positive;
    return config;
}

static void logic_ppe_guard(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results)
        return;
    if (!*ctx->state)
        *ctx->state = std::make_shared<GuardState>();
    GuardState &state = *std::static_pointer_cast<GuardState>(*ctx->state);

    if (!ctx->infer_enabled)
    {
        publish_idle(ctx);
        draw_text(ctx, "PPE穿戴检测: 推理已关闭", cv::Point(12, 24), cv::Scalar(180, 180, 180), 0.45, 1,
                  DrawCommand::ALL, true);
        return;
    }

    const std::string roi_name = ctx->param_string("roi_name");
    const int roi = roi_name.empty() ? ROI_FRAME : roi_find(ctx, roi_name.c_str());
    if (roi == ROI_NONE)
    {
        /* 区域名写错时不做“整帧兜底”，避免把全画面的人都算进作业区。 */
        publish_idle(ctx);
        draw_text(ctx, "作业区域不存在: 请检查 roi_name", cv::Point(12, 24), cv::Scalar(0, 165, 255), 0.45, 1,
                  DrawCommand::ALL, true);
        return;
    }

    ppe_guard::Config config = load_config(ctx);
    if (state.score_override > 0.0f)
        config.violation_min_score = state.score_override;

    /* ROI 过滤只影响参与判定的人员及其姿态结果；其余检测结果原样保留。 */
    std::vector<AlgoResult> roi_results;
    const std::vector<AlgoResult> *results = ctx->results;
    if (roi != ROI_FRAME)
    {
        roi_results.reserve(ctx->results->size());
        const std::string person_labels = ctx->param_string("person_labels");
        const std::vector<std::string> person_label_list = ppe_guard::split_labels(person_labels);
        for (const AlgoResult &result : *ctx->results)
        {
            const bool is_person = ppe_guard::matches_label(result.label, person_label_list);
            if ((is_person || result.is_coco17_pose()) && !roi_contains(ctx, result.box, roi))
                continue;
            roi_results.push_back(result);
        }
        results = &roi_results;
    }

    const ppe_guard::Outcome outcome = state.guard.update(*results, ctx->timestamp_ms, config);

    int confirmed_people = 0;
    for (const ppe_guard::Evaluation &person : outcome.people)
    {
        char badge[48];
        std::snprintf(badge, sizeof(badge), "#%d H%c T%c L%c R%c", person.track_id,
                      ppe_guard::status_char(person.status[ppe_guard::PART_HELMET]),
                      ppe_guard::status_char(person.status[ppe_guard::PART_WORKWEAR]),
                      ppe_guard::status_char(person.status[ppe_guard::PART_HAND_LEFT]),
                      ppe_guard::status_char(person.status[ppe_guard::PART_HAND_RIGHT]));

        bool any_candidate = false;
        for (int part = 0; part < ppe_guard::PART_COUNT; ++part)
            any_candidate = any_candidate || person.status[static_cast<size_t>(part)] == ppe_guard::PART_CANDIDATE;

        const cv::Scalar color = person.violation ? cv::Scalar(0, 0, 255)
                                 : any_candidate  ? cv::Scalar(0, 165, 255)
                                                  : cv::Scalar(0, 220, 0);
        draw_rect(ctx, person.box, color, 2);
        /* 徽标画在框的下边缘外侧：引擎在框顶画了 "ID x 类别"，不能被徽标压住。 */
        draw_text(ctx, badge, cv::Point(person.box.x, person.box.y + person.box.height + 13), color, 0.4, 1,
                  DrawCommand::ALL, true);

        if (!person.violation)
            continue;
        ++confirmed_people;

        /* 先画后报：证据图带着本帧标注；同一会话只提交一次。 */
        if (!person.report_due)
            continue;
        EventRequest request;
        request.event_type = "logic_ppe_violation";
        request.message = "人员安全穿戴违规: " + person.violation_types;
        request.fields = {event_field("track_id", person.track_id),
                          event_field("violation_types", person.violation_types),
                          event_field("part_states", person.part_states), event_field("person_score", person.score)};
        if (report_event(ctx, request).accepted())
            state.guard.mark_reported(person.track_id);
    }

    /* 左上角汇总：两行小字，避免大字号压住画面里的检测框标签。 */
    const cv::Scalar summary_color = confirmed_people > 0 ? cv::Scalar(0, 0, 255) : cv::Scalar(240, 240, 240);
    char summary[192];
    std::snprintf(summary, sizeof(summary), "PPE 候选%d 确认%d 人员%d", outcome.candidate_observations,
                  confirmed_people, static_cast<int>(outcome.people.size()));
    if (!state.debug_visible)
    {
        ctx->publish_bool("ppe_alarm", confirmed_people > 0);
        ctx->publish_int("confirmed_people", confirmed_people);
        ctx->publish_int("candidate_observations", outcome.candidate_observations);
        ctx->publish_int("rejected_observations", outcome.rejected_observations);
        ctx->publish_int("duplicate_observations", outcome.duplicate_observations);
        ctx->publish_int("vetoed_observations", outcome.vetoed_observations);
        ctx->publish_int("tracked_people", static_cast<int64_t>(outcome.people.size()));
        return;
    }
    draw_text(ctx, summary, cv::Point(12, 24), summary_color, 0.45, 1, DrawCommand::ALL, true);

    char counters[192];
    std::snprintf(counters, sizeof(counters), "丢弃%d 去重%d 否决%d", outcome.rejected_observations,
                  outcome.duplicate_observations, outcome.vetoed_observations);
    draw_text(ctx, counters, cv::Point(12, 44), summary_color, 0.45, 1, DrawCommand::ALL, true);

    char tuning[192];
    std::snprintf(tuning, sizeof(tuning), "灵敏度%.2f", static_cast<double>(config.violation_min_score));
    draw_text(ctx, tuning, cv::Point(12, 64), cv::Scalar(180, 220, 255), 0.4, 1, DrawCommand::ALL, true);

    /* 类别名自检：模型换了 label_path 没换时，这里能一眼看出对不上。 */
    if (!outcome.people.empty())
        state.unmatched_frames = 0;
    else if (!results->empty())
        ++state.unmatched_frames;
    if (state.unmatched_frames > 25)
        draw_text(ctx, "有检测框但没有人员类别: 检查 person_labels 与模型 label_path", cv::Point(12, 84),
                  cv::Scalar(0, 0, 255), 0.45, 1, DrawCommand::ALL, true);

    /* 本帧实际出现的类别名与数量：判断类名是否和权重对得上。 */
    {
        std::vector<std::pair<std::string, int>> counts;
        for (const AlgoResult &result : *ctx->results)
        {
            bool found = false;
            for (size_t i = 0; i < counts.size(); ++i)
                if (counts[i].first == result.label)
                {
                    ++counts[i].second;
                    found = true;
                    break;
                }
            if (!found)
                counts.push_back(std::make_pair(result.label, 1));
        }
        std::stable_sort(counts.begin(), counts.end(),
                         [](const std::pair<std::string, int> &lhs, const std::pair<std::string, int> &rhs) {
                             return lhs.second > rhs.second;
                         });
        std::string tally = "标签:";
        char item[96];
        for (size_t i = 0; i < counts.size() && i < 4; ++i)
        {
            std::snprintf(item, sizeof(item), " %s%d", counts[i].first.c_str(), counts[i].second);
            tally += item;
        }
        const cv::Mat *frame = ctx->business_frame();
        const int tally_y = (frame != nullptr && !frame->empty()) ? frame->rows - 10 : 620;
        draw_text(ctx, tally.c_str(), cv::Point(12, tally_y), cv::Scalar(200, 255, 200), 0.4, 1, DrawCommand::ALL,
                  true);
    }

    ctx->publish_bool("ppe_alarm", confirmed_people > 0);
    ctx->publish_int("confirmed_people", confirmed_people);
    ctx->publish_int("candidate_observations", outcome.candidate_observations);
    ctx->publish_int("rejected_observations", outcome.rejected_observations);
    ctx->publish_int("duplicate_observations", outcome.duplicate_observations);
    ctx->publish_int("vetoed_observations", outcome.vetoed_observations);
    ctx->publish_int("tracked_people", static_cast<int64_t>(outcome.people.size()));
}

static LogicActionResult logic_ppe_guard_action(ChannelContext *ctx, const LogicAction *action)
{
    if (!ctx || !ctx->state || !action)
        return {false, "ctx or action is null"};
    if (!*ctx->state)
        *ctx->state = std::make_shared<GuardState>();
    GuardState &state = *std::static_pointer_cast<GuardState>(*ctx->state);

    if (action->name == "ppe_reset_alarm")
    {
        state.guard.reset();
        return {true, "已清除全部人员的确认状态与证据"};
    }
    if (action->name == "ppe_toggle_debug")
    {
        state.debug_visible = !state.debug_visible;
        return {true, state.debug_visible ? "调试信息已显示" : "调试信息已隐藏"};
    }
    if (action->name == "ppe_sensitivity_up" || action->name == "ppe_sensitivity_down")
    {
        if (state.score_override <= 0.0f)
            state.score_override = ctx->param_float("violation_min_score");
        state.score_override += (action->name == "ppe_sensitivity_up") ? 0.05f : -0.05f;
        if (state.score_override > 0.95f)
            state.score_override = 0.95f;
        if (state.score_override < 0.50f)
            state.score_override = 0.50f;
        char message[96];
        std::snprintf(message, sizeof(message), "报警置信度门槛 %.2f", static_cast<double>(state.score_override));
        return {true, message};
    }
    return {false, "unsupported action: " + action->name};
}

} // namespace

REGISTER_LOGIC(logic_ppe_guard);
REGISTER_LOGIC_ACTION(logic_ppe_guard, logic_ppe_guard_action);
