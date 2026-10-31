#include "ppe_guard.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ppe_guard
{
namespace
{

struct TrackedFramePerson
{
    const AlgoResult *detection = nullptr;
    const AlgoResult *pose = nullptr;
    PersonState *state = nullptr;
    std::array<bool, PART_COUNT> visible = {{false, false, false, false}};
    std::array<bool, PART_COUNT> violation_seen = {{false, false, false, false}};
    bool hand_unsided_seen = false;
    std::array<bool, PART_COUNT> positive_seen = {{false, false, false, false}};
    bool head_checkable = false; /* 头部区域没被别人挡住，可用于“正类缺失”推断 */
};

bool keypoint_visible(const AlgoResult &pose, int index, float min_score)
{
    return index >= 0 && index < static_cast<int>(pose.keypoint_scores.size()) &&
           pose.keypoint_scores[static_cast<size_t>(index)] >= min_score;
}

cv::Point2f keypoint_at(const AlgoResult &pose, int index)
{
    if (index < 0 || index >= static_cast<int>(pose.keypoints.size()))
        return cv::Point2f(-1.0f, -1.0f);
    return pose.keypoints[static_cast<size_t>(index)];
}

float box_iou(const cv::Rect &lhs, const cv::Rect &rhs)
{
    const int left = std::max(lhs.x, rhs.x);
    const int top = std::max(lhs.y, rhs.y);
    const int right = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
    const int bottom = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
    if (right <= left || bottom <= top)
        return 0.0f;
    const float intersection = static_cast<float>((right - left) * (bottom - top));
    const float union_area =
        static_cast<float>(lhs.area()) + static_cast<float>(rhs.area()) - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

/* COCO17: 0 鼻, 1/2 眼, 5/6 肩, 9 左腕, 10 右腕, 11/12 髋。
 * 没有可用姿态时全部部位记未知：宁可不出结论，也不拿人体框猜装备归属。 */
void fill_visibility(const AlgoResult *pose, float min_score, std::array<bool, PART_COUNT> &visible)
{
    visible = {{false, false, false, false}};
    if (pose == nullptr)
        return;
    const bool head = keypoint_visible(*pose, 0, min_score) ||
                      (keypoint_visible(*pose, 1, min_score) && keypoint_visible(*pose, 2, min_score));
    const bool shoulders = keypoint_visible(*pose, 5, min_score) && keypoint_visible(*pose, 6, min_score);
    const bool hips = keypoint_visible(*pose, 11, min_score) || keypoint_visible(*pose, 12, min_score);
    visible[PART_HELMET] = head;
    visible[PART_WORKWEAR] = shoulders && hips;
    visible[PART_HAND_LEFT] = keypoint_visible(*pose, 9, min_score);
    visible[PART_HAND_RIGHT] = keypoint_visible(*pose, 10, min_score);
}

/* 手套框的三种去向：分到某只手、分不清左右（记不分侧证据）、不是手（丢弃）。 */
const int HAND_UNRESOLVED = -1;
const int HAND_REJECTED = -2;

float person_height(const TrackedFramePerson &person)
{
    return static_cast<float>(std::max(1, person.detection->box.height));
}

/* 违规框归到哪只手：两腕都可见时按距离判定，距离接近则不猜；
 * 距离超过 glove_wrist_ratio × 人高时不认为是手，直接丢弃。
 * 没有腕点就无法确认“这是手”，开启几何门控后同样丢弃。 */
int resolve_hand_side(const TrackedFramePerson &person, const cv::Rect &box, const Config &config)
{
    const bool left_ok = person.visible[PART_HAND_LEFT];
    const bool right_ok = person.visible[PART_HAND_RIGHT];
    if ((!left_ok && !right_ok) || person.pose == nullptr)
        return config.geometry_gate ? HAND_REJECTED : HAND_UNRESOLVED;

    const float max_distance = config.glove_wrist_ratio * person_height(person);
    const cv::Point center(box.x + box.width / 2, box.y + box.height / 2);
    const cv::Point2f left = keypoint_at(*person.pose, 9);
    const cv::Point2f right = keypoint_at(*person.pose, 10);
    const float dl = std::hypot(static_cast<float>(center.x) - left.x, static_cast<float>(center.y) - left.y);
    const float dr = std::hypot(static_cast<float>(center.x) - right.x, static_cast<float>(center.y) - right.y);

    if (left_ok != right_ok)
    {
        const float distance = left_ok ? dl : dr;
        if (config.geometry_gate && distance > max_distance)
            return HAND_REJECTED;
        return left_ok ? PART_HAND_LEFT : PART_HAND_RIGHT;
    }

    if (config.geometry_gate && dl > max_distance && dr > max_distance)
        return HAND_REJECTED;

    const float span = std::hypot(left.x - right.x, left.y - right.y);
    if (std::fabs(dl - dr) < 0.35f * std::max(span, 1.0f))
        return HAND_UNRESOLVED;
    return dl < dr ? PART_HAND_LEFT : PART_HAND_RIGHT;
}

/* 部位几何门控：框必须长在该部位该在的地方，否则这条证据不算数。
 * 安全帽：上部指定高度内 + 面积占人框的比例合理 + 贴近鼻/眼关键点；
 * 工作服：躯干区间内；手套：在 resolve_hand_side 里按腕点距离判定。 */
bool geometry_ok(int part, const cv::Rect &box, const TrackedFramePerson &person, const Config &config)
{
    if (!config.geometry_gate)
        return true;

    const cv::Rect &person_box = person.detection->box;
    if (person_box.width < 2 || person_box.height < 2)
        return false;

    const float height = person_height(person);
    const float center_x = static_cast<float>(box.x) + static_cast<float>(box.width) / 2.0f;
    const float center_y = static_cast<float>(box.y) + static_cast<float>(box.height) / 2.0f;
    const float relative_y = (center_y - static_cast<float>(person_box.y)) / height;

    if (part == PART_HELMET)
    {
        /* 允许负值：头部框可以高于人员框上边缘，但不能高出一个安全帽的合理范围。 */
        if (relative_y > config.helmet_top_ratio || relative_y < -config.helmet_top_ratio)
            return false;

        const float area_ratio =
            static_cast<float>(box.area()) / static_cast<float>(std::max(1, person_box.area()));
        if (area_ratio < config.helmet_area_min_ratio || area_ratio > config.helmet_area_max_ratio)
            return false;

        if (person.pose == nullptr)
            return true; /* 没有姿态时只能靠位置与面积约束 */

        float best = -1.0f;
        for (int index = 0; index <= 2; ++index) /* COCO17: 0 鼻, 1 左眼, 2 右眼 */
        {
            if (!keypoint_visible(*person.pose, index, config.keypoint_min_score))
                continue;
            const cv::Point2f joint = keypoint_at(*person.pose, index);
            const float dx = center_x - joint.x;
            const float dy = center_y - joint.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (best < 0.0f || distance < best)
                best = distance;
        }
        return best < 0.0f || best <= config.helmet_head_ratio * height;
    }

    if (part == PART_WORKWEAR)
        return relative_y >= config.workwear_top_ratio && relative_y <= config.workwear_bottom_ratio;

    return true;
}

/* 归属范围：安全帽框经常比人员框还高——检测框从颈肩开始时，头部框的中心落在人员框上方，
 * 若只看“中心是否在人员框内”，头部证据会被整体丢弃。因此安全帽项把人员框向上扩展
 * helmet_top_ratio 个人高，水平范围不动，避免跨人归属。 */
/* 安全帽框的搜索区：人体框向上抬 helmet_search_up_ratio、左右各放宽 helmet_search_side_ratio。
 * 人体检测框常常只框到肩颈，帽子在框顶之上；用严格 containment 会把“戴了”判成“没戴”。 */
cv::Rect helmet_search_region(const cv::Rect &person_box, const Config &config)
{
    const float up = config.helmet_search_up_ratio > 0.0f ? config.helmet_search_up_ratio : 0.6f;
    const float side = config.helmet_search_side_ratio >= 0.0f ? config.helmet_search_side_ratio : 0.15f;
    const int lift = static_cast<int>(static_cast<float>(person_box.height) * up);
    const int pad = static_cast<int>(static_cast<float>(person_box.width) * side);
    return cv::Rect(person_box.x - pad, person_box.y - lift, person_box.width + pad * 2,
                    person_box.height + lift);
}

/* 安全帽只归给“搜索区包含其中心、且头部中心离它最近”的那个人；并列时算归属不明。 */
int nearest_person_for_helmet(const cv::Rect &box, const std::vector<TrackedFramePerson> &people,
                              const Config &config)
{
    const cv::Point center(box.x + box.width / 2, box.y + box.height / 2);
    const float head_ratio = config.helmet_top_ratio > 0.0f ? config.helmet_top_ratio : 0.35f;
    int match = -1;
    double best = 0.0;
    int matches = 0;
    for (size_t index = 0; index < people.size(); ++index)
    {
        const cv::Rect &person_box = people[index].detection->box;
        if (!helmet_search_region(person_box, config).contains(center))
            continue;
        const cv::Point head_center(person_box.x + person_box.width / 2,
                                    person_box.y + static_cast<int>(person_box.height * head_ratio) / 2);
        const double dx = static_cast<double>(center.x - head_center.x);
        const double dy = static_cast<double>(center.y - head_center.y);
        const double dist = dx * dx + dy * dy;
        if (match < 0 || dist < best)
        {
            best = dist;
            match = static_cast<int>(index);
            matches = 1;
        }
        else if (dist == best)
        {
            ++matches;
        }
    }
    if (matches > 1)
        return -1;
    return match;
}

/* 多人挨着站时，安全帽框只归一个人的话，另一人会被判“没戴”。
 * 这里改成共享信用：搜索区内有安全帽框的人，全部记“戴了”。宁可漏报，不要误报。 */
void credit_helmet_shared(const cv::Rect &box, std::vector<TrackedFramePerson> &people, const Config &config)
{
    const cv::Point center(box.x + box.width / 2, box.y + box.height / 2);
    for (size_t index = 0; index < people.size(); ++index)
    {
        if (helmet_search_region(people[index].detection->box, config).contains(center))
            people[index].positive_seen[PART_HELMET] = true;
    }
}

int unique_person_for_box(const cv::Rect &box, const std::vector<TrackedFramePerson> &people, int part,
                          const Config &config)
{
    const cv::Point center(box.x + box.width / 2, box.y + box.height / 2);
    int match = -1;
    for (size_t index = 0; index < people.size(); ++index)
    {
        cv::Rect target = people[index].detection->box;
        if (config.geometry_gate && part == PART_HELMET)
        {
            const int slack = static_cast<int>(config.helmet_top_ratio * static_cast<float>(target.height));
            target.y -= slack;
            target.height += slack;
        }
        if (!target.contains(center))
            continue;
        if (match >= 0)
            return -1; /* 同时落在多个人员框内 → 归属不明 */
        match = static_cast<int>(index);
    }
    return match;
}

/* 优先同 track_id 的姿态结果，其次取框中心落在人员框内的姿态结果。 */
const AlgoResult *find_pose_for(const AlgoResult &person, const std::vector<AlgoResult> &results)
{
    const AlgoResult *fallback = nullptr;
    for (const AlgoResult &result : results)
    {
        if (!result.is_coco17_pose())
            continue;
        if (person.track_id >= 0 && result.track_id == person.track_id)
            return &result;
        if (fallback == nullptr && person.box.contains(result.box_center()))
            fallback = &result;
    }
    return fallback;
}

void prune_evidence(std::deque<uint64_t> &times, uint64_t now, uint64_t window_ms)
{
    while (!times.empty() && now >= times.front() && now - times.front() > window_ms)
        times.pop_front();
}

/* 相邻帧（含同一帧多条）不重复计证据；时间回退时丢弃旧时间基准。 */
void push_evidence(std::deque<uint64_t> &times, uint64_t now, uint64_t gap_ms)
{
    if (!times.empty() && now < times.back())
    {
        times.clear();
        times.push_back(now);
        return;
    }
    if (times.empty() || now - times.back() >= gap_ms)
        times.push_back(now);
}

bool evidence_confirmed(const std::deque<uint64_t> &times, size_t required_observations, uint64_t required_span_ms)
{
    return required_observations >= 1 && times.size() >= required_observations && times.size() >= 2 &&
           times.back() - times.front() >= required_span_ms;
}

/* 正类框（例如 helmet/glove）落在某人的对应部位上：说明该部位已经穿戴，
 * 同一帧里再出现的负类框不能作数。模型常在同一个物体上同时给出两个互斥类别。 */
void collect_positive_parts(const std::vector<AlgoResult> &results, std::vector<TrackedFramePerson> &people,
                            const Config &config)
{
    if (!config.positive_veto)
        return;
    for (const AlgoResult &result : results)
    {
        if (result.score < config.violation_min_score)
            continue;
        for (int part = 0; part < PART_COUNT; ++part)
        {
            if (!config.enabled[static_cast<size_t>(part)])
                continue;
            if (!matches_label(result.label, config.positive_labels[static_cast<size_t>(part)]))
                continue;
            if (part == PART_HELMET)
            {
                credit_helmet_shared(result.box, people, config);
                continue;
            }
            const int person_index = unique_person_for_box(result.box, people, part, config);
            if (person_index < 0)
                continue;
            TrackedFramePerson &person = people[static_cast<size_t>(person_index)];
            if (part == PART_HAND_LEFT || part == PART_HAND_RIGHT)
            {
                const int side = resolve_hand_side(person, result.box, config);
                if (side < 0)
                    continue;
                person.positive_seen[static_cast<size_t>(side)] = true;
                continue;
            }
            person.positive_seen[static_cast<size_t>(part)] = true;
        }
    }
}

} // namespace

std::vector<std::string> split_labels(const std::string &text)
{
    std::vector<std::string> labels;
    size_t begin = 0;
    while (begin <= text.size())
    {
        const size_t end = text.find(',', begin);
        const size_t stop = (end == std::string::npos) ? text.size() : end;
        size_t first = begin;
        size_t last = stop;
        while (first < last && text[first] == ' ')
            ++first;
        while (last > first && text[last - 1] == ' ')
            --last;
        if (last > first)
            labels.push_back(text.substr(first, last - first));
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return labels;
}

bool matches_label(const std::string &label, const std::vector<std::string> &labels)
{
    for (const std::string &candidate : labels)
        if (label == candidate)
            return true;
    return false;
}

const char *part_full_name(int part)
{
    switch (part)
    {
    case PART_HELMET:
        return "未戴安全帽";
    case PART_WORKWEAR:
        return "未穿工作服";
    case PART_HAND_LEFT:
        return "未戴左手套";
    case PART_HAND_RIGHT:
        return "未戴右手套";
    default:
        break;
    }
    return "未知违规";
}

char status_char(int status)
{
    switch (status)
    {
    case PART_VIOLATION:
        return 'X';
    case PART_CANDIDATE:
        return '?';
    case PART_CLEAR:
        return '.';
    default:
        break;
    }
    return '-';
}

Outcome Guard::update(const std::vector<AlgoResult> &results, uint64_t now_ms, const Config &config)
{
    Outcome outcome;

    if (clock_initialised_ && now_ms < last_ms_)
        people_.clear(); /* 时间回退：旧证据的时间基准失效 */
    clock_initialised_ = true;
    last_ms_ = now_ms;

    std::vector<TrackedFramePerson> people;
    people.reserve(people_.size());
    for (const AlgoResult &result : results)
    {
        if (result.track_id < 0 || result.score < config.person_min_score)
            continue;
        if (!matches_label(result.label, config.person_labels))
            continue;

        bool duplicate = false;
        for (const TrackedFramePerson &existing : people)
            duplicate = duplicate || existing.detection->track_id == result.track_id;
        if (duplicate)
            continue; /* 同一帧同一 track 只取首个框，避免重复计证据 */

        PersonState &person_state = people_[result.track_id];
        if (person_state.last_seen_ms != 0 && now_ms >= person_state.last_seen_ms &&
            now_ms - person_state.last_seen_ms > config.reacquire_gap_ms)
            person_state = PersonState{}; /* 断开过久：按新的作业会话重新积累 */
        person_state.last_seen_ms = now_ms;

        TrackedFramePerson person;
        person.detection = &result;
        person.pose = find_pose_for(result, results);
        person.state = &person_state;
        fill_visibility(person.pose, config.keypoint_min_score, person.visible);
        people.push_back(person);
    }

    /* 正类框先登记：同一个部位上“穿好了”的证据优先，负类框不再作数。 */
    collect_positive_parts(results, people, config);

    /* “头部区域里没有安全帽框”只有在头部本身可核验时才算证据：
     * 前面还站着别人（人框盖住头部）时，看不见不等于没戴。 */
    if (config.helmet_infer_by_absence && config.enabled[PART_HELMET])
    {
        const float head_ratio = config.helmet_top_ratio > 0.0f ? config.helmet_top_ratio : 0.35f;
        for (size_t i = 0; i < people.size(); ++i)
        {
            const cv::Rect &box = people[i].detection->box;
            if (box.height < 60)
                continue; /* 人太小，头部区域只有几个像素，推断不可靠 */
            if (box.y < 6)
                continue; /* 头顶贴画面边缘，头部可能被裁掉，看不见不等于没戴 */
            if (people[i].detection->score < config.violation_min_score)
                continue; /* 人框本身不可信，不拿它推断“没戴” */
            const cv::Rect head(box.x, box.y, box.width,
                                std::max(1, static_cast<int>(box.height * head_ratio)));
            bool covered = false;
            for (size_t j = 0; j < people.size() && !covered; ++j)
            {
                if (j == i)
                    continue;
                const cv::Rect &other = people[j].detection->box;
                const int overlap_x = std::max(0, std::min(head.x + head.width, other.x + other.width) -
                                                      std::max(head.x, other.x));
                const int overlap_y = std::max(0, std::min(head.y + head.height, other.y + other.height) -
                                                      std::max(head.y, other.y));
                if (static_cast<float>(overlap_x * overlap_y) > 0.4f * static_cast<float>(head.area()))
                    covered = true;
            }
            people[i].head_checkable = !covered;
        }
    }

    /* 违规框 → 同类去重 → 唯一人员框 → 部位。归属不明或跨帧不一致的框直接丢弃。 */
    std::vector<const AlgoResult *> violations;
    violations.reserve(results.size());
    for (const AlgoResult &result : results)
    {
        if (result.score < config.violation_min_score)
            continue;
        bool wanted = false;
        for (int part = 0; part < PART_COUNT && !wanted; ++part)
            wanted = config.enabled[static_cast<size_t>(part)] &&
                     matches_label(result.label, config.violation_labels[static_cast<size_t>(part)]);
        if (wanted)
            violations.push_back(&result);
    }
    /* 同一物品被输出多个重叠框时只留最高分，别让一帧凑出多条证据。 */
    if (config.dedupe_iou > 0.0f && violations.size() > 1)
    {
        std::stable_sort(violations.begin(), violations.end(),
                         [](const AlgoResult *lhs, const AlgoResult *rhs) { return lhs->score > rhs->score; });
        std::vector<const AlgoResult *> kept;
        for (const AlgoResult *candidate : violations)
        {
            bool duplicate = false;
            for (const AlgoResult *existing : kept)
            {
                if (existing->label == candidate->label &&
                    box_iou(existing->box, candidate->box) >= config.dedupe_iou)
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
            {
                ++outcome.duplicate_observations;
                continue;
            }
            kept.push_back(candidate);
        }
        violations.swap(kept);
    }

    for (const AlgoResult *candidate : violations)
    {
        const AlgoResult &result = *candidate;
        int matched_part = -1;
        for (int part = 0; part < PART_COUNT; ++part)
        {
            if (!config.enabled[static_cast<size_t>(part)] ||
                !matches_label(result.label, config.violation_labels[static_cast<size_t>(part)]))
                continue;
            matched_part = part;
            break;
        }
        if (matched_part < 0)
            continue;

        const int person_index = unique_person_for_box(result.box, people, matched_part, config);
        if (person_index < 0)
        {
            ++outcome.rejected_observations; /* 不在任何人身上，或同时落在两个人身上 */
            continue;
        }
        TrackedFramePerson &person = people[static_cast<size_t>(person_index)];
        if (person.detection->frame_id != result.frame_id)
        {
            ++outcome.rejected_observations;
            continue;
        }

        if (matched_part == PART_HAND_LEFT || matched_part == PART_HAND_RIGHT)
        {
            const int side = resolve_hand_side(person, result.box, config);
            if (side >= 0)
            {
                if (config.positive_veto && person.positive_seen[static_cast<size_t>(side)])
                {
                    ++outcome.vetoed_observations; /* 这只手上已经看到手套了 */
                    continue;
                }
                if (config.require_pose_for_violation && !person.visible[static_cast<size_t>(side)])
                {
                    ++outcome.rejected_observations; /* 腕点不可见：可能戴着或挡着 */
                    continue;
                }
                person.violation_seen[static_cast<size_t>(side)] = true;
            }
            else if (side == HAND_UNRESOLVED)
                person.hand_unsided_seen = true;
            else
                ++outcome.rejected_observations; /* 离腕点太远或没有腕点：不是手 */
            continue;
        }

        if (config.positive_veto && person.positive_seen[static_cast<size_t>(matched_part)])
        {
            ++outcome.vetoed_observations; /* 同一部位上已识别到正类装备 */
            continue;
        }
        if (config.require_pose_for_violation && !person.visible[static_cast<size_t>(matched_part)])
        {
            ++outcome.rejected_observations; /* 该部位不可见：遮挡或出画，不下结论 */
            continue;
        }
        if (!geometry_ok(matched_part, result.box, person, config))
        {
            ++outcome.rejected_observations; /* 部位位置或尺寸不合理 */
            continue;
        }
        person.violation_seen[static_cast<size_t>(matched_part)] = true;
    }

    for (TrackedFramePerson &person : people)
    {
        if (config.helmet_infer_by_absence && config.enabled[PART_HELMET] && person.head_checkable &&
            !person.positive_seen[PART_HELMET])
            person.violation_seen[PART_HELMET] = true; /* 头部可核验，但没有安全帽框 */

        PersonState &person_state = *person.state;
        Evaluation evaluation;
        evaluation.track_id = person.detection->track_id;
        evaluation.box = person.detection->box;
        evaluation.score = person.detection->score;

        for (int part = 0; part < PART_COUNT; ++part)
        {
            PartEvidence &evidence = person_state.parts[static_cast<size_t>(part)];
            prune_evidence(evidence.violations, now_ms, config.evidence_window_ms);
            if (person.violation_seen[static_cast<size_t>(part)])
            {
                ++outcome.candidate_observations;
                push_evidence(evidence.violations, now_ms, config.evidence_gap_ms);
            }
            if (!evidence.confirmed &&
                evidence_confirmed(evidence.violations, config.required_observations, config.required_span_ms))
                evidence.confirmed = true;

            if (!config.enabled[static_cast<size_t>(part)])
                evaluation.status[static_cast<size_t>(part)] = PART_UNKNOWN;
            else if (evidence.confirmed)
                evaluation.status[static_cast<size_t>(part)] = PART_VIOLATION;
            else if (!evidence.violations.empty())
                evaluation.status[static_cast<size_t>(part)] = PART_CANDIDATE;
            else if (person.visible[static_cast<size_t>(part)])
                evaluation.status[static_cast<size_t>(part)] = PART_CLEAR;
            else
                evaluation.status[static_cast<size_t>(part)] = PART_UNKNOWN;
        }

        /* 分不清左右手的裸手证据：只用于“手套”这一项，不分配给具体某只手。 */
        prune_evidence(person_state.hand_unsided.violations, now_ms, config.evidence_window_ms);
        if (person.hand_unsided_seen)
        {
            ++outcome.candidate_observations;
            push_evidence(person_state.hand_unsided.violations, now_ms, config.evidence_gap_ms);
        }
        if (!person_state.hand_unsided.confirmed &&
            evidence_confirmed(person_state.hand_unsided.violations, config.required_observations,
                               config.required_span_ms))
            person_state.hand_unsided.confirmed = true;
        evaluation.gloves_unsided = config.enabled[PART_HAND_LEFT] && person_state.hand_unsided.confirmed;

        for (int part = 0; part < PART_COUNT; ++part)
        {
            if (evaluation.status[static_cast<size_t>(part)] != PART_VIOLATION)
                continue;
            evaluation.violation = true;
            if (!evaluation.violation_types.empty())
                evaluation.violation_types += ", ";
            evaluation.violation_types += part_full_name(part);
        }
        if (evaluation.gloves_unsided)
        {
            evaluation.violation = true;
            if (!evaluation.violation_types.empty())
                evaluation.violation_types += ", ";
            evaluation.violation_types += "未戴手套(未分左右手)";
        }
        evaluation.report_due = evaluation.violation && !person_state.reported;

        char part_states[64];
        std::snprintf(part_states, sizeof(part_states), "H=%c,T=%c,L=%c,R=%c",
                      status_char(evaluation.status[PART_HELMET]), status_char(evaluation.status[PART_WORKWEAR]),
                      status_char(evaluation.status[PART_HAND_LEFT]), status_char(evaluation.status[PART_HAND_RIGHT]));
        evaluation.part_states = part_states;

        outcome.people.push_back(evaluation);
    }

    for (auto it = people_.begin(); it != people_.end();)
    {
        if (now_ms > it->second.last_seen_ms && now_ms - it->second.last_seen_ms > config.state_ttl_ms)
            it = people_.erase(it);
        else
            ++it;
    }

    return outcome;
}

void Guard::mark_reported(int track_id)
{
    auto it = people_.find(track_id);
    if (it != people_.end())
        it->second.reported = true;
}

} // namespace ppe_guard
