#include "logic/core/logic_common.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace
{
constexpr const char *POSE_MODEL_TYPE = "yolov8_pose";
constexpr const char *SCRAPER_MODEL_TYPE = "yolov5_seg";
constexpr int PERSON_CLASS_ID = 0;
constexpr int SCRAPER_CLASS_ID = 0;
constexpr double PI = 3.14159265358979323846;

const cv::Scalar COLOR_NORMAL(0, 220, 255);
const cv::Scalar COLOR_WATER(255, 128, 0);
const cv::Scalar COLOR_FALL(0, 0, 255);
const cv::Scalar COLOR_HELP(255, 0, 255);
const cv::Scalar COLOR_EXCLUDED(160, 160, 160);

struct CandidateSnapshot
{
    int64_t seq = 0;
    uint64_t unix_ms = 0;
    int64_t track_id = -1;
    double evidence_quality = 0.0;
    double view_scraper_mask_ratio = 0.0;
    double person_scraper_overlap_ratio = 0.0;
};

struct TrackState
{
    int64_t internal_id = 0;
    int platform_track_id = -1;
    cv::Rect box;
    int missing_frames = 0;
    std::deque<uint8_t> fall_history;
    std::deque<uint8_t> upright_history;
    std::deque<uint8_t> corridor_history;
    std::deque<double> left_wrist_history;
    std::deque<double> right_wrist_history;
    bool fall_transition_seen = false;
    bool fall_active = false;
    int fall_reset_streak = 0;
    bool help_active = false;
    int help_reset_streak = 0;
    bool has_previous = false;
    double previous_center_y = 0.0;
    double previous_box_height = 1.0;
    uint64_t previous_seen_ms = 0;
};

struct DetectionState
{
    std::deque<uint8_t> water_history;
    bool water_active = false;
    int water_empty_streak = 0;
    std::map<int64_t, TrackState> tracks;
    int64_t next_internal_id = 1;
    CandidateSnapshot water_candidate;
    CandidateSnapshot fall_candidate;
    CandidateSnapshot help_candidate;
};

struct PoseFrameAssessment
{
    AlgoResult *result = nullptr;
    bool in_water = false;
    bool in_corridor = false;
    bool valid_water = false;
    bool fall_positive = false;
    bool help_positive = false;
    double scraper_overlap = 0.0;
    double evidence_quality = 0.0;
    double aspect_ratio = 0.0;
    double torso_angle = -1.0;
    double drop_speed = 0.0;
    int64_t track_id = -1;
};

DetectionState &detection_state(ChannelContext *ctx)
{
    if (!*ctx->state)
        *ctx->state = std::make_shared<DetectionState>();
    return *std::static_pointer_cast<DetectionState>(*ctx->state);
}

bool matches_source(const AlgoResult &result, const char *type, const std::string &model_id)
{
    return result.model_type == type && (model_id.empty() || result.model_id == model_id);
}

const RoiZone *valid_roi(const ChannelContext *ctx, const std::string &name)
{
    const RoiZone *roi = ctx && !name.empty() ? ctx->roi_by_name(name.c_str()) : nullptr;
    return roi && roi->polygon.size() >= 3 ? roi : nullptr;
}

bool point_in_roi(const RoiZone *roi, const cv::Point2f &point)
{
    return roi && cv::pointPolygonTest(roi->polygon, point, false) >= 0.0;
}

const cv::Mat *find_scraper_mask(const std::vector<AlgoResult> &results, const std::string &model_id)
{
    for (const AlgoResult &result : results)
        if (matches_source(result, SCRAPER_MODEL_TYPE, model_id) && !result.boxMask.empty())
            return &result.boxMask;
    return nullptr;
}

cv::Mat build_scraper_mask(const std::vector<AlgoResult> &results, const std::string &model_id, const cv::Mat &raw_mask,
                           double min_score)
{
    cv::Mat filtered = cv::Mat::zeros(raw_mask.size(), CV_8UC1);
    const cv::Rect bounds(0, 0, raw_mask.cols, raw_mask.rows);
    for (const AlgoResult &result : results)
    {
        if (!matches_source(result, SCRAPER_MODEL_TYPE, model_id) || result.class_id != SCRAPER_CLASS_ID ||
            result.score < min_score)
            continue;
        const cv::Rect box = result.box & bounds;
        if (box.empty())
            continue;
        cv::Mat selected;
        cv::compare(raw_mask(box), SCRAPER_CLASS_ID + 1, selected, cv::CMP_EQ);
        filtered(box).setTo(SCRAPER_CLASS_ID + 1, selected);
    }
    return filtered;
}

double mask_ratio_in_box(const cv::Mat *mask, const cv::Rect &box)
{
    if (!mask || mask->empty())
        return 0.0;
    const cv::Rect clipped = box & cv::Rect(0, 0, mask->cols, mask->rows);
    if (clipped.empty())
        return 0.0;
    cv::Mat selected;
    cv::compare((*mask)(clipped), SCRAPER_CLASS_ID + 1, selected, cv::CMP_EQ);
    return static_cast<double>(cv::countNonZero(selected)) / static_cast<double>(clipped.area());
}

double full_mask_ratio(const cv::Mat *mask)
{
    if (!mask || mask->empty())
        return 0.0;
    cv::Mat selected;
    cv::compare(*mask, SCRAPER_CLASS_ID + 1, selected, cv::CMP_EQ);
    return static_cast<double>(cv::countNonZero(selected)) / static_cast<double>(mask->total());
}

bool keypoint(const AlgoResult &pose, int index, double threshold, cv::Point2f *point)
{
    if (!point || index < 0 || static_cast<std::size_t>(index) >= pose.keypoints.size())
        return false;
    if (static_cast<std::size_t>(index) < pose.keypoint_scores.size() &&
        pose.keypoint_scores[static_cast<std::size_t>(index)] < threshold)
        return false;
    const cv::Point2f value = pose.keypoints[static_cast<std::size_t>(index)];
    if (!std::isfinite(value.x) || !std::isfinite(value.y))
        return false;
    *point = value;
    return true;
}

bool midpoint(const AlgoResult &pose, int first, int second, double threshold, cv::Point2f *point)
{
    cv::Point2f a, b;
    const bool has_a = keypoint(pose, first, threshold, &a);
    const bool has_b = keypoint(pose, second, threshold, &b);
    if (!has_a && !has_b)
        return false;
    *point = has_a && has_b ? (a + b) * 0.5F : (has_a ? a : b);
    return true;
}

double pose_quality(const AlgoResult &pose, double threshold)
{
    if (pose.keypoints.empty())
        return 0.0;
    int valid = 0;
    for (std::size_t i = 0; i < pose.keypoints.size(); ++i)
    {
        cv::Point2f ignored;
        if (keypoint(pose, static_cast<int>(i), threshold, &ignored))
            ++valid;
    }
    return static_cast<double>(valid) / static_cast<double>(pose.keypoints.size());
}

struct HandSamples
{
    int raised_count = 0;
    double left = std::numeric_limits<double>::quiet_NaN();
    double right = std::numeric_limits<double>::quiet_NaN();
};

HandSamples raised_hand_samples(const AlgoResult &pose, double threshold)
{
    HandSamples samples;
    const double box_width = std::max(pose.box.width, 1);
    const double raised_margin = std::max(pose.box.height, 1) * 0.04;
    const auto collect = [&](int shoulder_index, int wrist_index, double *output) {
        cv::Point2f shoulder, wrist;
        if (!output || !keypoint(pose, shoulder_index, threshold, &shoulder) ||
            !keypoint(pose, wrist_index, threshold, &wrist))
            return;
        if (wrist.y < shoulder.y - raised_margin)
        {
            *output = (wrist.x - shoulder.x) / box_width;
            ++samples.raised_count;
        }
    };
    collect(5, 9, &samples.left);
    collect(6, 10, &samples.right);
    return samples;
}

struct WaveStats
{
    int raised_frames = 0;
    double swing_ratio = 0.0;
    int direction_changes = 0;
};

WaveStats measure_wave(const std::deque<double> &history, double min_swing_ratio)
{
    WaveStats stats;
    std::vector<double> values;
    values.reserve(history.size());
    for (double value : history)
        if (std::isfinite(value))
            values.push_back(value);
    stats.raised_frames = static_cast<int>(values.size());
    if (values.empty())
        return stats;

    const auto range = std::minmax_element(values.begin(), values.end());
    stats.swing_ratio = *range.second - *range.first;
    const double minimum_step = std::max(0.02, min_swing_ratio / 5.0);
    std::vector<int> directions;
    for (std::size_t i = 1; i < values.size(); ++i)
    {
        const double difference = values[i] - values[i - 1];
        if (std::abs(difference) >= minimum_step)
            directions.push_back(difference > 0.0 ? 1 : -1);
    }
    for (std::size_t i = 1; i < directions.size(); ++i)
        stats.direction_changes += directions[i] != directions[i - 1];
    return stats;
}

bool is_waving(const std::deque<double> &history, int min_raised_frames, double min_swing_ratio,
               int min_direction_changes)
{
    const WaveStats stats = measure_wave(history, min_swing_ratio);
    return stats.raised_frames >= min_raised_frames && stats.swing_ratio >= min_swing_ratio &&
           stats.direction_changes >= min_direction_changes;
}

cv::Point2f ground_point(const AlgoResult &pose, double threshold)
{
    cv::Point2f point;
    if (midpoint(pose, 15, 16, threshold, &point) || midpoint(pose, 11, 12, threshold, &point))
        return point;
    return cv::Point2f(static_cast<float>(pose.box.x + pose.box.width * 0.5),
                       static_cast<float>(pose.box.y + pose.box.height));
}

double torso_angle(const AlgoResult &pose, double threshold)
{
    cv::Point2f shoulder, hip;
    if (!midpoint(pose, 5, 6, threshold, &shoulder) || !midpoint(pose, 11, 12, threshold, &hip))
        return -1.0;
    const double dx = std::abs(static_cast<double>(hip.x - shoulder.x));
    const double dy = std::abs(static_cast<double>(hip.y - shoulder.y));
    return dx == 0.0 && dy == 0.0 ? -1.0 : std::atan2(dx, dy) * 180.0 / PI;
}

double box_iou(const cv::Rect &a, const cv::Rect &b)
{
    const cv::Rect overlap = a & b;
    const double union_area = static_cast<double>(a.area()) + b.area() - overlap.area();
    return union_area > 0.0 ? overlap.area() / union_area : 0.0;
}

TrackState *match_track(DetectionState &state, const AlgoResult &pose, const std::set<int64_t> &used,
                        double iou_threshold, double center_threshold)
{
    if (pose.track_id >= 0)
        for (auto &entry : state.tracks)
            if (!used.count(entry.first) && entry.second.platform_track_id == pose.track_id)
                return &entry.second;

    TrackState *best = nullptr;
    double best_score = -std::numeric_limits<double>::infinity();
    for (auto &entry : state.tracks)
    {
        if (used.count(entry.first))
            continue;
        const double iou = box_iou(pose.box, entry.second.box);
        const cv::Point2f a(pose.box.x + pose.box.width * 0.5F, pose.box.y + pose.box.height * 0.5F);
        const cv::Point2f b(entry.second.box.x + entry.second.box.width * 0.5F,
                            entry.second.box.y + entry.second.box.height * 0.5F);
        const double scale =
            std::max({pose.box.width, pose.box.height, entry.second.box.width, entry.second.box.height, 1});
        const double distance = cv::norm(a - b) / scale;
        if (iou < iou_threshold && distance > center_threshold)
            continue;
        const double score = iou * 2.0 + std::max(0.0, 1.0 - distance);
        if (score > best_score)
        {
            best_score = score;
            best = &entry.second;
        }
    }
    return best;
}

int deque_hits(const std::deque<uint8_t> &history)
{
    int hits = 0;
    for (uint8_t value : history)
        hits += value != 0;
    return hits;
}

void trim(std::deque<uint8_t> &history, int size)
{
    while (static_cast<int>(history.size()) > size)
        history.pop_front();
}

void publish(ChannelContext *ctx, const DetectionState &state, const std::string &hardware_id, bool water_roi_available,
             bool corridor_roi_available, bool mask_available, int raw_person_count, int valid_water_person_count,
             bool fall_active, bool help_active, int water_hits, double view_mask_ratio)
{
    ctx->publish_string("hardware_id", hardware_id);
    ctx->publish_bool("water_roi_available", water_roi_available);
    ctx->publish_bool("corridor_roi_available", corridor_roi_available);
    ctx->publish_bool("scraper_mask_available", mask_available);
    ctx->publish_int("raw_person_count", raw_person_count);
    ctx->publish_int("valid_water_person_count", valid_water_person_count);
    ctx->publish_int("water_history_hits", water_hits);
    ctx->publish_bool("water_active", state.water_active);
    ctx->publish_bool("fall_active", fall_active);
    ctx->publish_bool("help_active", help_active);
    ctx->publish_number("view_scraper_mask_ratio", view_mask_ratio);
    ctx->publish_int("water_candidate_seq", state.water_candidate.seq);
    ctx->publish_int("water_candidate_unix_ms", static_cast<int64_t>(state.water_candidate.unix_ms));
    ctx->publish_int("water_candidate_track_id", state.water_candidate.track_id);
    ctx->publish_number("water_evidence_quality", state.water_candidate.evidence_quality);
    ctx->publish_number("water_person_scraper_overlap_ratio", state.water_candidate.person_scraper_overlap_ratio);
    ctx->publish_int("fall_candidate_seq", state.fall_candidate.seq);
    ctx->publish_int("fall_candidate_unix_ms", static_cast<int64_t>(state.fall_candidate.unix_ms));
    ctx->publish_int("fall_candidate_track_id", state.fall_candidate.track_id);
    ctx->publish_number("fall_evidence_quality", state.fall_candidate.evidence_quality);
    ctx->publish_int("help_candidate_seq", state.help_candidate.seq);
    ctx->publish_int("help_candidate_unix_ms", static_cast<int64_t>(state.help_candidate.unix_ms));
    ctx->publish_int("help_candidate_track_id", state.help_candidate.track_id);
    ctx->publish_number("help_evidence_quality", state.help_candidate.evidence_quality);
}
} // namespace

static void logic_fall_detection(ChannelContext *ctx)
{
    if (!ctx || !ctx->state || !ctx->results)
        return;

    DetectionState &state = detection_state(ctx);
    const std::string pose_model_id = ctx->param_string("pose_model_id");
    const std::string scraper_model_id = ctx->param_string("scraper_model_id");
    const std::string hardware_id = ctx->param_string("hardware_id");
    const RoiZone *water_roi = valid_roi(ctx, ctx->param_string("water_roi_name"));
    const RoiZone *corridor_roi = valid_roi(ctx, ctx->param_string("corridor_roi_name"));
    const double min_person_score = ctx->param_float("min_person_confidence");
    const double min_scraper_score = ctx->param_float("min_scraper_confidence");
    const double max_scraper_overlap = ctx->param_float("max_person_scraper_overlap_ratio");
    const double keypoint_threshold = ctx->param_float("keypoint_confidence");
    const int water_window = static_cast<int>(ctx->param_int("water_window_frames"));
    const int water_confirm = static_cast<int>(ctx->param_int("water_confirm_frames"));
    const int water_reset = static_cast<int>(ctx->param_int("water_reset_frames"));
    const int fall_window = static_cast<int>(ctx->param_int("fall_window_frames"));
    const int fall_confirm = static_cast<int>(ctx->param_int("fall_confirm_frames"));
    const int fall_reset = static_cast<int>(ctx->param_int("fall_reset_frames"));
    const int max_missing = static_cast<int>(ctx->param_int("max_track_missing_frames"));
    const double fall_aspect = ctx->param_float("fall_aspect_ratio");
    const double fall_angle = ctx->param_float("fall_torso_angle_degrees");
    const double fall_speed = ctx->param_float("fall_drop_speed");
    const bool help_enabled = ctx->param_bool("help_enabled");
    const int help_window = static_cast<int>(ctx->param_int("help_window_frames"));
    const int help_raised = static_cast<int>(ctx->param_int("help_min_raised_frames"));
    const double help_swing = ctx->param_float("help_min_swing_ratio");
    const int help_changes = static_cast<int>(ctx->param_int("help_min_direction_changes"));
    const int help_reset = static_cast<int>(ctx->param_int("help_reset_frames"));
    const double track_iou = ctx->param_float("track_iou_threshold");
    const double track_distance = ctx->param_float("track_center_distance_ratio");

    const cv::Mat *raw_mask = find_scraper_mask(*ctx->results, scraper_model_id);
    cv::Mat filtered_mask;
    const cv::Mat *scraper_mask = nullptr;
    if (raw_mask && raw_mask->channels() == 1)
    {
        filtered_mask = build_scraper_mask(*ctx->results, scraper_model_id, *raw_mask, min_scraper_score);
        scraper_mask = &filtered_mask;
    }
    const bool mask_available = scraper_mask != nullptr;
    const double view_mask_ratio = full_mask_ratio(scraper_mask);

    for (auto &entry : state.tracks)
        ++entry.second.missing_frames;

    int raw_person_count = 0;
    int valid_water_person_count = 0;
    std::vector<PoseFrameAssessment> assessments;
    std::set<int64_t> used_tracks;
    PoseFrameAssessment best_water;
    bool has_best_water = false;
    PoseFrameAssessment best_fall;
    bool has_new_fall = false;
    PoseFrameAssessment best_help;
    bool has_new_help = false;

    for (AlgoResult &pose : *ctx->results)
    {
        if (!matches_source(pose, POSE_MODEL_TYPE, pose_model_id) || pose.class_id != PERSON_CLASS_ID)
            continue;
        ++raw_person_count;
        if (pose.score < min_person_score || pose.box.empty())
            continue;

        TrackState *track = match_track(state, pose, used_tracks, track_iou, track_distance);
        if (!track)
        {
            TrackState created;
            created.internal_id = state.next_internal_id++;
            created.box = pose.box;
            created.platform_track_id = pose.track_id;
            state.tracks.emplace(created.internal_id, created);
            track = &state.tracks.find(created.internal_id)->second;
        }
        used_tracks.insert(track->internal_id);
        track->platform_track_id = pose.track_id >= 0 ? pose.track_id : track->platform_track_id;
        track->missing_frames = 0;

        PoseFrameAssessment item;
        item.result = &pose;
        item.track_id = track->platform_track_id >= 0 ? track->platform_track_id : track->internal_id;
        item.evidence_quality = pose_quality(pose, keypoint_threshold);
        item.scraper_overlap = mask_ratio_in_box(scraper_mask, pose.box);

        // “作业人员”仅表示当前人员框未被刮泥臂掩码大面积覆盖，不代表身份识别结果。
        char person_label[128];
        if (!mask_available)
            std::snprintf(person_label, sizeof(person_label), "作业人员:待确认  重叠率:--");
        else
            std::snprintf(person_label, sizeof(person_label), "作业人员:%s  重叠率:%.1f%%",
                          item.scraper_overlap <= max_scraper_overlap ? "是" : "否", item.scraper_overlap * 100.0);
        pose.label = person_label;

        item.in_water = point_in_roi(water_roi, pose.box_center());
        item.valid_water = item.in_water && item.scraper_overlap <= max_scraper_overlap;
        if (item.valid_water)
        {
            ++valid_water_person_count;
            if (!has_best_water || item.evidence_quality > best_water.evidence_quality)
            {
                best_water = item;
                has_best_water = true;
            }
        }

        item.in_corridor = point_in_roi(corridor_roi, ground_point(pose, keypoint_threshold));
        item.aspect_ratio = static_cast<double>(pose.box.width) / std::max(pose.box.height, 1);
        item.torso_angle = torso_angle(pose, keypoint_threshold);
        const bool was_in_corridor = deque_hits(track->corridor_history) > 0;
        track->corridor_history.push_back(item.in_corridor ? 1U : 0U);
        trim(track->corridor_history, std::max(fall_window * 2, 5));

        if (track->has_previous && ctx->timestamp_ms > track->previous_seen_ms)
        {
            const double elapsed_seconds = (ctx->timestamp_ms - track->previous_seen_ms) / 1000.0;
            const double center_y = pose.box.y + pose.box.height * 0.5;
            item.drop_speed =
                std::max(0.0, (center_y - track->previous_center_y) / track->previous_box_height / elapsed_seconds);
        }
        const bool horizontal =
            item.aspect_ratio >= fall_aspect || (item.torso_angle >= 0.0 && item.torso_angle >= fall_angle);
        const bool upright = item.aspect_ratio < std::min(0.9, fall_aspect) &&
                             (item.torso_angle < 0.0 || item.torso_angle < std::min(40.0, fall_angle));
        const bool had_upright = deque_hits(track->upright_history) > 0;
        track->upright_history.push_back(upright ? 1U : 0U);
        trim(track->upright_history, std::max(fall_window * 2, 5));

        if (corridor_roi && (item.in_corridor || was_in_corridor) && horizontal &&
            (had_upright || item.drop_speed >= fall_speed))
            track->fall_transition_seen = true;
        item.fall_positive =
            corridor_roi && track->fall_transition_seen && (item.in_corridor || was_in_corridor) && horizontal;
        track->fall_history.push_back(item.fall_positive ? 1U : 0U);
        trim(track->fall_history, fall_window);

        if (!track->fall_active && deque_hits(track->fall_history) >= fall_confirm)
        {
            track->fall_active = true;
            track->fall_reset_streak = 0;
            if (!has_new_fall || item.evidence_quality > best_fall.evidence_quality)
            {
                best_fall = item;
                has_new_fall = true;
            }
        }
        else if (track->fall_active && item.fall_positive)
            track->fall_reset_streak = 0;
        else if (track->fall_active && ++track->fall_reset_streak >= fall_reset)
        {
            track->fall_active = false;
            track->fall_transition_seen = false;
            track->fall_reset_streak = 0;
            track->fall_history.clear();
            track->upright_history.clear();
        }

        // 求救识别与原 Python 算法保持一致：手腕高于肩部后，按人体框宽度归一化水平摆幅。
        const HandSamples hand_samples = raised_hand_samples(pose, keypoint_threshold);
        track->left_wrist_history.push_back(hand_samples.left);
        track->right_wrist_history.push_back(hand_samples.right);
        while (static_cast<int>(track->left_wrist_history.size()) > help_window)
            track->left_wrist_history.pop_front();
        while (static_cast<int>(track->right_wrist_history.size()) > help_window)
            track->right_wrist_history.pop_front();
        item.help_positive =
            help_enabled && (is_waving(track->left_wrist_history, help_raised, help_swing, help_changes) ||
                             is_waving(track->right_wrist_history, help_raised, help_swing, help_changes));

        if (!track->help_active && item.help_positive)
        {
            track->help_active = true;
            track->help_reset_streak = 0;
            if (!has_new_help || item.evidence_quality > best_help.evidence_quality)
            {
                best_help = item;
                has_new_help = true;
            }
        }
        else if (track->help_active && item.help_positive)
            track->help_reset_streak = 0;
        else if (track->help_active && ++track->help_reset_streak >= help_reset)
        {
            track->help_active = false;
            track->help_reset_streak = 0;
            track->left_wrist_history.clear();
            track->right_wrist_history.clear();
        }

        track->box = pose.box;
        track->previous_center_y = pose.box.y + pose.box.height * 0.5;
        track->previous_box_height = std::max(pose.box.height, 1);
        track->previous_seen_ms = ctx->timestamp_ms;
        track->has_previous = true;
        assessments.push_back(item);
    }

    for (auto it = state.tracks.begin(); it != state.tracks.end();)
        if (it->second.missing_frames > max_missing)
            it = state.tracks.erase(it);
        else
            ++it;

    const bool water_positive = water_roi && valid_water_person_count > 0;
    if (water_roi)
    {
        state.water_history.push_back(water_positive ? 1U : 0U);
        trim(state.water_history, water_window);
        if (!state.water_active && deque_hits(state.water_history) >= water_confirm)
        {
            state.water_active = true;
            state.water_empty_streak = 0;
            ++state.water_candidate.seq;
            state.water_candidate.unix_ms = ctx->unix_ms;
            state.water_candidate.track_id = has_best_water ? best_water.track_id : -1;
            // 与原算法一致，入水证据质量取“画面未被刮泥机遮挡的比例”。
            state.water_candidate.evidence_quality = std::max(0.0, 1.0 - view_mask_ratio);
            state.water_candidate.view_scraper_mask_ratio = view_mask_ratio;
            state.water_candidate.person_scraper_overlap_ratio = has_best_water ? best_water.scraper_overlap : 0.0;
        }
        else if (state.water_active && water_positive)
            state.water_empty_streak = 0;
        else if (state.water_active && ++state.water_empty_streak >= water_reset)
        {
            state.water_active = false;
            state.water_empty_streak = 0;
            state.water_history.clear();
        }
    }
    else
    {
        state.water_active = false;
        state.water_empty_streak = 0;
        state.water_history.clear();
    }

    if (has_new_fall)
    {
        ++state.fall_candidate.seq;
        state.fall_candidate.unix_ms = ctx->unix_ms;
        state.fall_candidate.track_id = best_fall.track_id;
        state.fall_candidate.evidence_quality = best_fall.evidence_quality;
        state.fall_candidate.view_scraper_mask_ratio = view_mask_ratio;
    }
    if (has_new_help)
    {
        ++state.help_candidate.seq;
        state.help_candidate.unix_ms = ctx->unix_ms;
        state.help_candidate.track_id = best_help.track_id;
        state.help_candidate.evidence_quality = best_help.evidence_quality;
        state.help_candidate.view_scraper_mask_ratio = view_mask_ratio;
    }

    bool fall_active = false;
    bool help_active = false;
    int fall_hits = 0;
    int fall_reset_progress = 0;
    int help_reset_progress = 0;
    WaveStats best_wave;
    for (const auto &entry : state.tracks)
    {
        const TrackState &track = entry.second;
        fall_active = fall_active || track.fall_active;
        help_active = help_active || track.help_active;
        fall_hits = std::max(fall_hits, deque_hits(track.fall_history));
        if (track.fall_active)
            fall_reset_progress = std::max(fall_reset_progress, track.fall_reset_streak);
        if (track.help_active)
            help_reset_progress = std::max(help_reset_progress, track.help_reset_streak);

        const WaveStats left = measure_wave(track.left_wrist_history, help_swing);
        const WaveStats right = measure_wave(track.right_wrist_history, help_swing);
        for (const WaveStats &current : {left, right})
        {
            if (current.raised_frames > best_wave.raised_frames ||
                (current.raised_frames == best_wave.raised_frames && current.swing_ratio > best_wave.swing_ratio) ||
                (current.raised_frames == best_wave.raised_frames && current.swing_ratio == best_wave.swing_ratio &&
                 current.direction_changes > best_wave.direction_changes))
                best_wave = current;
        }
    }

    for (PoseFrameAssessment &item : assessments)
    {
        if (item.fall_positive)
            item.result->box_color = COLOR_FALL;
        else if (item.help_positive)
            item.result->box_color = COLOR_HELP;
        else if (item.valid_water)
            item.result->box_color = COLOR_WATER;
        else if (item.in_water)
            item.result->box_color = COLOR_EXCLUDED;
    }

    const int water_hits = deque_hits(state.water_history);
    if (ctx->param_bool("show_status"))
    {
        char line[256];
        // 面向现场人员显示实时进度：命中值是当前滑动窗口内的阳性帧数，复位值是连续阴性帧数。
        std::snprintf(line, sizeof(line), "落水[%s] 命中%d/%d(确认%d) 复位%d/%d", state.water_active ? "报警" : "观察",
                      water_hits, water_window, water_confirm, state.water_empty_streak, water_reset);
        draw_text(ctx, line, cv::Point(16, 30), state.water_active ? COLOR_FALL : COLOR_NORMAL, 0.58, 2,
                  DrawCommand::ALL, true);

        std::snprintf(line, sizeof(line), "跌倒[%s] 命中%d/%d(确认%d) 复位%d/%d", fall_active ? "报警" : "观察",
                      fall_hits, fall_window, fall_confirm, fall_reset_progress, fall_reset);
        draw_text(ctx, line, cv::Point(16, 60), fall_active ? COLOR_FALL : COLOR_NORMAL, 0.58, 2, DrawCommand::ALL,
                  true);

        // 未启用挥手识别时完全隐藏呼救状态，避免让使用者误以为该能力正在运行。
        if (help_enabled)
        {
            std::snprintf(line, sizeof(line), "呼救[%s] 举手%d/%d(需%d) 摆幅%.2f/%.2f 变向%d/%d 复位%d/%d",
                          help_active ? "报警" : "观察", best_wave.raised_frames, help_window, help_raised,
                          best_wave.swing_ratio, help_swing, best_wave.direction_changes, help_changes,
                          help_reset_progress, help_reset);
            draw_text(ctx, line, cv::Point(16, 90), help_active ? COLOR_FALL : COLOR_NORMAL, 0.52, 2, DrawCommand::ALL,
                      true);
        }
    }

    publish(ctx, state, hardware_id, water_roi != nullptr, corridor_roi != nullptr, mask_available, raw_person_count,
            valid_water_person_count, fall_active, help_active, water_hits, view_mask_ratio);
}

REGISTER_LOGIC(logic_fall_detection);
