/* ppe_guard 回归测试：只依赖 ppe_guard.h/.cpp，用合成帧验证部位状态机与证据窗口。
 * 构建：-DBUILD_PPE_TESTS=ON；运行：./build/engine/ppe_tests 或 ctest -R ppe_regression。 */

#include "ppe_guard.h"

#include <cstdio>
#include <utility>
#include <vector>

namespace
{

int g_checks = 0;
int g_failures = 0;

void check_impl(bool ok, const char *expr, int line)
{
    ++g_checks;
    if (ok)
    {
        return;
    }
    ++g_failures;
    std::printf("FAIL line %d: %s\n", line, expr);
}

#define CHECK(cond) check_impl((cond), #cond, __LINE__)

using ppe_guard::Config;
using ppe_guard::Evaluation;
using ppe_guard::Guard;
using ppe_guard::Outcome;

AlgoResult person_box(int track_id, cv::Rect box, float score = 0.9f)
{
    AlgoResult result;
    result.label = "person";
    result.box = box;
    result.track_id = track_id;
    result.score = score;
    result.frame_id = 1;
    return result;
}

AlgoResult violation_box(const char *label, cv::Rect box, float score = 0.9f)
{
    AlgoResult result;
    result.label = label;
    result.box = box;
    result.track_id = -1;
    result.score = score;
    result.frame_id = 1;
    return result;
}

/* 只给需要的关键点打分，其余置为不可见，用于构造遮挡与可见性场景。 */
AlgoResult pose_box(int track_id, const std::vector<std::pair<int, cv::Point2f>> &points)
{
    AlgoResult result;
    result.label = "pose";
    result.box = cv::Rect(0, 0, 100, 200);
    result.track_id = track_id;
    result.score = 0.9f;
    result.frame_id = 1;
    result.pose_schema = PoseKeypointSchema::Coco17;
    result.keypoints.assign(17, cv::Point2f(0.0f, 0.0f));
    result.keypoint_scores.assign(17, 0.05f);
    for (size_t index = 0; index < points.size(); ++index)
    {
        const size_t joint = static_cast<size_t>(points[index].first);
        result.keypoints[joint] = points[index].second;
        result.keypoint_scores[joint] = 0.9f;
    }
    return result;
}

AlgoResult fully_visible_pose(int track_id, cv::Point2f left_wrist, cv::Point2f right_wrist)
{
    std::vector<std::pair<int, cv::Point2f>> points;
    points.push_back(std::make_pair(0, cv::Point2f(50.0f, 20.0f)));
    points.push_back(std::make_pair(1, cv::Point2f(45.0f, 15.0f)));
    points.push_back(std::make_pair(2, cv::Point2f(55.0f, 15.0f)));
    points.push_back(std::make_pair(5, cv::Point2f(30.0f, 60.0f)));
    points.push_back(std::make_pair(6, cv::Point2f(70.0f, 60.0f)));
    points.push_back(std::make_pair(9, left_wrist));
    points.push_back(std::make_pair(10, right_wrist));
    points.push_back(std::make_pair(11, cv::Point2f(35.0f, 150.0f)));
    points.push_back(std::make_pair(12, cv::Point2f(65.0f, 150.0f)));
    return pose_box(track_id, points);
}

Config base_config()
{
    Config config;
    config.person_labels.push_back("person");
    config.violation_labels[ppe_guard::PART_HELMET].push_back("no_helmet");
    config.violation_labels[ppe_guard::PART_HELMET].push_back("bare_head");
    config.violation_labels[ppe_guard::PART_WORKWEAR].push_back("no_workwear");
    config.violation_labels[ppe_guard::PART_HAND_LEFT].push_back("bare_hand");
    config.violation_labels[ppe_guard::PART_HAND_RIGHT].push_back("bare_hand");
    config.person_min_score = 0.35f;
    config.violation_min_score = 0.5f;
    config.keypoint_min_score = 0.3f;
    config.evidence_window_ms = 4000;
    config.evidence_gap_ms = 250;
    config.required_observations = 3;
    config.required_span_ms = 1000;
    config.reacquire_gap_ms = 3000;
    config.state_ttl_ms = 600000;
    return config;
}

const Evaluation &only_person(const Outcome &outcome)
{
    return outcome.people.at(0);
}

int status_of(const Evaluation &evaluation, int part)
{
    return evaluation.status[static_cast<size_t>(part)];
}

/* 目标：3 次观测还不够，必须同时满足次数与 1 秒跨度。 */
void test_confirmation_needs_count_and_span()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(7, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20)));

    Outcome outcome = guard.update(frame, 0, config);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_CANDIDATE);
    CHECK(!only_person(outcome).violation);
    CHECK(!only_person(outcome).report_due);

    guard.update(frame, 400, config);
    outcome = guard.update(frame, 800, config);
    CHECK(!outcome.people.at(0).violation); /* 已有 3 条独立证据，但跨度只有 800ms */

    outcome = guard.update(frame, 1200, config);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_VIOLATION);
    CHECK(only_person(outcome).violation);
    CHECK(only_person(outcome).report_due);
    CHECK(only_person(outcome).violation_types == "未戴安全帽");
    CHECK(only_person(outcome).part_states == "H=X,T=-,L=-,R=-");

    guard.mark_reported(7);
    outcome = guard.update(frame, 1600, config);
    CHECK(only_person(outcome).violation);
    CHECK(!only_person(outcome).report_due); /* 同一会话不再重复上报 */
}

/* 目标：相邻 100ms 的重复帧只能算一条证据，不能靠连续三帧凑出确认。 */
void test_gap_dedupes_adjacent_frames()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(3, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20)));

    guard.update(frame, 0, config);
    guard.update(frame, 100, config);
    Outcome outcome = guard.update(frame, 200, config);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_CANDIDATE);
    CHECK(!only_person(outcome).violation);
    CHECK(outcome.candidate_observations == 1); /* 本帧只有 1 个部位存在违规观测 */

    outcome = guard.update(frame, 1600, config); /* 隔开足够才成为第 2 条独立证据 */
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_CANDIDATE);
    CHECK(!only_person(outcome).violation);

    outcome = guard.update(frame, 2900, config);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_VIOLATION);
    CHECK(only_person(outcome).violation);
}

/* 目标：窗口外的旧证据过期，不能与很久之后的观测拼成一次违规。 */
void test_window_expiry()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(5, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20)));

    guard.update(frame, 0, config);
    guard.update(frame, 500, config);
    const Outcome outcome = guard.update(frame, 6000, config);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_CANDIDATE);
    CHECK(!only_person(outcome).violation);
}

/* 目标：没有姿态结果时全部部位未知，不拿人体框猜装备。 */
void test_occlusion_marks_unknown()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(9, cv::Rect(0, 0, 100, 200)));

    const Outcome outcome = guard.update(frame, 0, config);
    for (int part = 0; part < ppe_guard::PART_COUNT; ++part)
    {
        CHECK(status_of(only_person(outcome), part) == ppe_guard::PART_UNKNOWN);
    }
    CHECK(only_person(outcome).part_states == "H=-,T=-,L=-,R=-");
}

/* 目标：部位可核验且未见违规时是 clear，必须与未知区分开。 */
void test_visible_parts_are_clear()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(11, cv::Rect(0, 0, 100, 200)));
    frame.push_back(fully_visible_pose(11, cv::Point2f(20.0f, 100.0f), cv::Point2f(80.0f, 100.0f)));

    const Outcome outcome = guard.update(frame, 0, config);
    for (int part = 0; part < ppe_guard::PART_COUNT; ++part)
    {
        CHECK(status_of(only_person(outcome), part) == ppe_guard::PART_CLEAR);
    }
    CHECK(!only_person(outcome).violation);
}

/* 目标：关键点被遮挡时只把对应部位降为未知，其余部位照常判断。 */
void test_partial_visibility()
{
    Guard guard;
    const Config config = base_config();
    std::vector<std::pair<int, cv::Point2f>> points;
    points.push_back(std::make_pair(5, cv::Point2f(30.0f, 60.0f)));
    points.push_back(std::make_pair(6, cv::Point2f(70.0f, 60.0f)));
    points.push_back(std::make_pair(11, cv::Point2f(35.0f, 150.0f)));
    points.push_back(std::make_pair(12, cv::Point2f(65.0f, 150.0f)));

    std::vector<AlgoResult> frame;
    frame.push_back(person_box(13, cv::Rect(0, 0, 100, 200)));
    frame.push_back(pose_box(13, points));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_WORKWEAR) == ppe_guard::PART_CLEAR);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HAND_LEFT) == ppe_guard::PART_UNKNOWN);
}

/* 目标：违规框同时落在两个人框内，归属不明，谁都不算。 */
void test_ambiguous_attribution_is_dropped()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(21, cv::Rect(0, 0, 100, 200)));
    frame.push_back(person_box(22, cv::Rect(50, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(65, 0, 20, 20))); /* 中心 (75,10) 同时在两人框内 */

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.people.size() == 2);
    CHECK(outcome.candidate_observations == 0);
    CHECK(!outcome.people.at(0).violation);
    CHECK(!outcome.people.at(1).violation);
}

/* 目标：裸手框落在哪个腕点，就只记哪只手。 */
void test_hand_side_resolution()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(31, cv::Rect(0, 0, 100, 200)));
    frame.push_back(fully_visible_pose(31, cv::Point2f(20.0f, 100.0f), cv::Point2f(80.0f, 100.0f)));
    frame.push_back(violation_box("bare_hand", cv::Rect(10, 90, 20, 20))); /* 中心 (20,100) 即左腕 */

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HAND_LEFT) == ppe_guard::PART_CANDIDATE);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HAND_RIGHT) == ppe_guard::PART_CLEAR);
    CHECK(!only_person(outcome).gloves_unsided);
}

/* 目标：两腕距离接近时不猜左右手，只记“不分侧”的手套证据。 */
void test_ambiguous_hand_side()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(41, cv::Rect(0, 0, 100, 200)));
    frame.push_back(fully_visible_pose(41, cv::Point2f(40.0f, 100.0f), cv::Point2f(60.0f, 100.0f)));
    frame.push_back(violation_box("bare_hand", cv::Rect(40, 90, 20, 20))); /* 中心 (50,100) 到两腕等距 */

    guard.update(frame, 0, config);
    guard.update(frame, 500, config);
    const Outcome outcome = guard.update(frame, 1100, config);

    CHECK(only_person(outcome).gloves_unsided);
    CHECK(only_person(outcome).violation);
    CHECK(only_person(outcome).violation_types == "未戴手套(未分左右手)");
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HAND_LEFT) != ppe_guard::PART_VIOLATION);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HAND_RIGHT) != ppe_guard::PART_VIOLATION);
}

/* 目标：同一 track 断开超过 reacquire_gap 视为新会话，可以再次报警。 */
void test_reacquire_starts_new_session()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(51, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20)));

    guard.update(frame, 0, config);
    guard.update(frame, 500, config);
    Outcome outcome = guard.update(frame, 1100, config);
    CHECK(only_person(outcome).report_due);
    guard.mark_reported(51);

    outcome = guard.update(frame, 5000, config); /* 断连 3900ms > 3000ms */
    CHECK(!only_person(outcome).violation);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_CANDIDATE);

    guard.update(frame, 5500, config);
    outcome = guard.update(frame, 6000, config);
    CHECK(only_person(outcome).violation);
    CHECK(only_person(outcome).report_due);
}

/* 目标：同一帧同一 track 出现两次只算一个人。 */
void test_duplicate_track_in_one_frame()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(61, cv::Rect(0, 0, 100, 200)));
    frame.push_back(person_box(61, cv::Rect(5, 0, 100, 200), 0.7f));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.people.size() == 1);
}

/* 目标：时间回退时清空旧时间基准，避免无符号相减溢出成巨大跨度。 */
void test_timestamp_regression()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(71, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20)));

    guard.update(frame, 0, config);
    guard.update(frame, 500, config);
    guard.update(frame, 1100, config);

    std::vector<AlgoResult> quiet;
    quiet.push_back(person_box(71, cv::Rect(0, 0, 100, 200)));
    const Outcome outcome = guard.update(quiet, 100, config); /* 时间回退 */
    CHECK(!only_person(outcome).violation);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
}

/* 目标：关闭的检查项不产生候选，也不参与报警。 */
void test_disabled_part()
{
    Guard guard;
    Config config = base_config();
    config.enabled[ppe_guard::PART_HELMET] = false;
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(81, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20)));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
    CHECK(outcome.candidate_observations == 0);
    CHECK(!only_person(outcome).violation);
}

/* 目标：低置信度目标被过滤；标签必须完全相等才匹配。 */
void test_score_and_label_filter()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(91, cv::Rect(0, 0, 100, 200), 0.1f));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20)));
    CHECK(guard.update(frame, 0, config).people.empty());

    std::vector<AlgoResult> frame2;
    frame2.push_back(person_box(92, cv::Rect(0, 0, 100, 200)));
    frame2.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20), 0.1f));
    CHECK(guard.update(frame2, 0, config).candidate_observations == 0);

    std::vector<std::string> workwear_labels;
    workwear_labels.push_back("no_workwear");
    CHECK(!ppe_guard::matches_label("no_uniform", workwear_labels));
    CHECK(ppe_guard::matches_label("no_workwear", workwear_labels));

    const std::vector<std::string> labels = ppe_guard::split_labels(" person , head ,, ");
    CHECK(labels.size() == 2);
    CHECK(labels.at(0) == "person");
    CHECK(labels.at(1) == "head");
}

/* 目标：长期不出现的人员状态会被清理，避免内存增长。 */
void test_state_ttl()
{
    Guard guard;
    Config config = base_config();
    config.state_ttl_ms = 1000;
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(101, cv::Rect(0, 0, 100, 200)));
    guard.update(frame, 0, config);
    CHECK(guard.tracked_people() == 1);
    guard.update(std::vector<AlgoResult>(), 2000, config);
    CHECK(guard.tracked_people() == 0);
}

/* 目标：安全帽框长在腰部时几何门控丢弃，多帧也凑不出违规。 */
void test_helmet_geometry_rejects_low_box()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(111, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 110, 20, 20))); /* 中心 y=120 → 0.6 人高 */

    Outcome outcome;
    for (int index = 0; index < 4; ++index)
        outcome = guard.update(frame, static_cast<uint64_t>(index) * 500, config);

    CHECK(outcome.candidate_observations == 0);
    CHECK(outcome.rejected_observations == 1); /* 每帧各丢弃一次，计数按帧清零 */
    CHECK(!only_person(outcome).violation);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
}

/* 目标：面积占到人框 12% 以上的安全帽框丢弃（典型是把整个上身判成头部）。 */
void test_helmet_geometry_rejects_big_box()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(112, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(0, 0, 60, 60))); /* 面积比 0.18 */

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(outcome.rejected_observations == 1);
}

/* 目标：位置与面积都合理的安全帽框照常进入证据。 */
void test_helmet_geometry_accepts_top_box()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(113, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 6, 18, 18)));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.candidate_observations == 1);
    CHECK(outcome.rejected_observations == 0);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_CANDIDATE);
}

/* 目标：工作服框落在腿上时丢弃，落在躯干时进入证据。 */
void test_workwear_band()
{
    const Config config = base_config();
    std::vector<AlgoResult> leg_frame;
    leg_frame.push_back(person_box(121, cv::Rect(0, 0, 100, 200)));
    leg_frame.push_back(violation_box("no_workwear", cv::Rect(30, 170, 40, 20))); /* 中心 y=180 → 0.9 */

    Guard leg_guard;
    const Outcome leg_outcome = leg_guard.update(leg_frame, 0, config);
    CHECK(leg_outcome.candidate_observations == 0);
    CHECK(leg_outcome.rejected_observations == 1);

    std::vector<AlgoResult> torso_frame;
    torso_frame.push_back(person_box(122, cv::Rect(0, 0, 100, 200)));
    torso_frame.push_back(violation_box("no_workwear", cv::Rect(30, 90, 40, 20))); /* 中心 y=100 → 0.5 */

    Guard torso_guard;
    const Outcome torso_outcome = torso_guard.update(torso_frame, 0, config);
    CHECK(torso_outcome.candidate_observations == 1);
    CHECK(status_of(only_person(torso_outcome), ppe_guard::PART_WORKWEAR) == ppe_guard::PART_CANDIDATE);
}

/* 目标：手套框离腕点太远时丢弃，不能记成“不分侧”证据。 */
void test_glove_geometry_rejects_far_box()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(131, cv::Rect(0, 0, 100, 200)));
    frame.push_back(fully_visible_pose(131, cv::Point2f(20.0f, 100.0f), cv::Point2f(80.0f, 100.0f)));
    frame.push_back(violation_box("bare_hand", cv::Rect(10, 180, 20, 20))); /* 中心 (20,190) 距左腕 90px */

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(outcome.rejected_observations == 1);
    CHECK(!only_person(outcome).gloves_unsided);
}

/* 目标：没有腕点就无法确认“这是手”，手套项不产生证据。 */
void test_glove_requires_wrist()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(141, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("bare_hand", cv::Rect(40, 90, 20, 20)));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(outcome.rejected_observations == 1);
    CHECK(!only_person(outcome).gloves_unsided);
}

/* 目标：几何门控可关闭，用于现场对比“模型问题”还是“门控问题”。 */
void test_geometry_gate_can_be_disabled()
{
    Guard guard;
    Config config = base_config();
    config.geometry_gate = false;
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(151, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 110, 20, 20)));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.candidate_observations == 1);
    CHECK(outcome.rejected_observations == 0);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_CANDIDATE);
}

/* 边界说明：带安全帽的头被模型判成 bare_head，但框的位置和面积恰好合理时，
 * 几何门控不会拒掉它——这类误报只能靠模型或正类否决解决。本用例把这个边界固定下来，
 * 避免以后把它当成回归。 */
void test_helmet_geometry_keeps_correct_position_box()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(161, cv::Rect(355, 65, 115, 140)));
    frame.push_back(violation_box("bare_head", cv::Rect(368, 50, 28, 22))); /* 408/16100 = 2.5% */

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.rejected_observations == 0);
    CHECK(outcome.candidate_observations == 1);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_CANDIDATE);
}

/* 目标：中性的“头”类（bare_head）不在违规标签里时，戴帽场景不产生任何证据。
 * 这是现场踩过的坑：把中性头部类当成未戴帽，框位置再正确也会报警。 */
void test_neutral_head_label_is_not_a_violation()
{
    Guard guard;
    Config config = base_config();
    config.violation_labels[ppe_guard::PART_HELMET].clear();
    config.violation_labels[ppe_guard::PART_HELMET].push_back("no_helmet");

    std::vector<AlgoResult> frame;
    frame.push_back(person_box(171, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("bare_head", cv::Rect(40, 5, 20, 20)));

    Outcome outcome;
    for (int index = 0; index < 6; ++index)
        outcome = guard.update(frame, static_cast<uint64_t>(index) * 500, config);

    CHECK(outcome.candidate_observations == 0);
    CHECK(outcome.rejected_observations == 0);
    CHECK(!only_person(outcome).violation);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
}

/* 目标：同一物品被输出两个几乎重合的框时，只算一条证据。 */
void test_dedupe_drops_overlapping_boxes()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(9, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20), 0.95f));
    frame.push_back(violation_box("no_helmet", cv::Rect(41, 1, 20, 20), 0.80f));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.candidate_observations == 1);
    CHECK(outcome.duplicate_observations == 1);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_CANDIDATE);
}

/* 目标：位置不同、不重叠的同类框各自算数，去重不能把真违规吃掉。 */
void test_dedupe_keeps_distinct_boxes()
{
    Guard guard;
    const Config config = base_config();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(10, cv::Rect(0, 0, 100, 200)));
    frame.push_back(person_box(11, cv::Rect(120, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20)));
    frame.push_back(violation_box("no_helmet", cv::Rect(160, 0, 20, 20)));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.duplicate_observations == 0);
    CHECK(outcome.candidate_observations == 2);
}

/* 目标：0.75 门槛以下的框不产生任何证据。 */
void test_low_confidence_never_counts()
{
    Guard guard;
    Config config = base_config();
    config.violation_min_score = 0.75f;
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(12, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20), 0.70f));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
}

/* 目标：同一部位上有正类框时，负类框不再作数（戴了就不该报没戴）。 */
void test_positive_veto_blocks_violation()
{
    Guard guard;
    Config config = base_config();
    config.positive_labels[ppe_guard::PART_HELMET].push_back("helmet");
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(13, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("helmet", cv::Rect(40, 0, 20, 20)));
    frame.push_back(violation_box("no_helmet", cv::Rect(41, 1, 20, 20)));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(outcome.vetoed_observations == 1);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);

    Config no_veto = config;
    no_veto.positive_veto = false;
    Guard plain;
    const Outcome plain_outcome = plain.update(frame, 0, no_veto);
    CHECK(plain_outcome.candidate_observations == 1);
}

/* 目标：要求“部位可见”时，没有姿态结果就不采信证据。 */
void test_require_pose_blocks_violation()
{
    Guard guard;
    Config config = base_config();
    config.require_pose_for_violation = true;
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(14, cv::Rect(0, 0, 100, 200)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20)));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(outcome.rejected_observations == 1);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
}

/* 目标：要求“部位可见”且头部关键点可见时，证据正常入账。 */
void test_require_pose_allows_visible_part()
{
    Guard guard;
    Config config = base_config();
    config.require_pose_for_violation = true;
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(15, cv::Rect(0, 0, 100, 200)));
    frame.push_back(fully_visible_pose(15, cv::Point2f(20.0f, 100.0f), cv::Point2f(80.0f, 100.0f)));
    frame.push_back(violation_box("no_helmet", cv::Rect(40, 0, 20, 20)));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.candidate_observations == 1);
    CHECK(outcome.rejected_observations == 0);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_CANDIDATE);
}

/* 目标：模型只有“戴好了”的正类时，头部区域一直没有安全帽框 → 判未戴。 */
void test_helmet_absence_infers_violation()
{
    Guard guard;
    Config config = base_config();
    config.helmet_infer_by_absence = true;
    config.violation_labels[ppe_guard::PART_HELMET].clear();
    config.positive_labels[ppe_guard::PART_HELMET].push_back("helmet");
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(16, cv::Rect(0, 10, 100, 200)));

    guard.update(frame, 0, config);
    guard.update(frame, 400, config);
    Outcome outcome = guard.update(frame, 1200, config);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_VIOLATION);
    CHECK(only_person(outcome).violation_types == "未戴安全帽");

    guard.mark_reported(16);

    /* 一旦出现安全帽正类框，本帧不再计“缺失”证据。 */
    std::vector<AlgoResult> with_helmet = frame;
    with_helmet.push_back(violation_box("helmet", cv::Rect(40, 10, 20, 22)));
    const Outcome suppressed = guard.update(with_helmet, 1600, config);
    CHECK(suppressed.candidate_observations == 0);
    CHECK(status_of(only_person(suppressed), ppe_guard::PART_HELMET) == ppe_guard::PART_VIOLATION); /* 仍锁存 */
    CHECK(!only_person(suppressed).report_due); /* 已上报过，同会话不再推 */
}

/* 目标：前面还站着别人、头部区域被盖住时，“没看到安全帽”不能当证据。 */
void test_helmet_absence_blocked_by_occluder()
{
    Guard guard;
    Config config = base_config();
    config.helmet_infer_by_absence = true;
    config.violation_labels[ppe_guard::PART_HELMET].clear();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(17, cv::Rect(0, 0, 100, 200)));
    frame.push_back(person_box(18, cv::Rect(0, 0, 100, 200)));

    const Outcome outcome = guard.update(frame, 0, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(!outcome.people.at(0).violation);
    CHECK(!outcome.people.at(1).violation);
}

/* 目标：默认关闭时，没有负类就不会凭空判违规。 */
void test_helmet_absence_off_by_default()
{
    Guard guard;
    Config config = base_config();
    config.violation_labels[ppe_guard::PART_HELMET].clear();
    std::vector<AlgoResult> frame;
    frame.push_back(person_box(19, cv::Rect(0, 0, 100, 200)));

    guard.update(frame, 0, config);
    guard.update(frame, 500, config);
    const Outcome outcome = guard.update(frame, 1200, config);
    CHECK(!only_person(outcome).violation);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
}

/* 目标：缺帽推断同样受置信度约束，人框分数不够就不推断。 */
void test_helmet_absence_needs_confident_person()
{
    Guard guard;
    Config config = base_config();
    config.helmet_infer_by_absence = true;
    config.violation_labels[ppe_guard::PART_HELMET].clear();
    config.positive_labels[ppe_guard::PART_HELMET].push_back("helmet");
    config.violation_min_score = 0.75f;

    std::vector<AlgoResult> frame;
    frame.push_back(person_box(20, cv::Rect(0, 0, 100, 200), 0.60f));

    guard.update(frame, 0, config);
    guard.update(frame, 500, config);
    const Outcome outcome = guard.update(frame, 1200, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
}

/* 目标：帽子框在人体框顶之上几像素（人体框只框到肩颈）时仍算“戴了”。 */
void test_helmet_above_person_still_counts()
{
    Guard guard;
    Config config = base_config();
    config.helmet_infer_by_absence = true;
    config.violation_labels[ppe_guard::PART_HELMET].clear();
    config.positive_labels[ppe_guard::PART_HELMET].push_back("helmet");

    std::vector<AlgoResult> frame;
    frame.push_back(person_box(21, cv::Rect(100, 120, 120, 300)));
    frame.push_back(violation_box("helmet", cv::Rect(150, 95, 40, 30)));

    guard.update(frame, 0, config);
    guard.update(frame, 500, config);
    const Outcome outcome = guard.update(frame, 1200, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
}

/* 目标：人框贴画面顶边（头可能被裁掉）时不做缺帽推断。 */
void test_helmet_absence_skips_edge_person()
{
    Guard guard;
    Config config = base_config();
    config.helmet_infer_by_absence = true;
    config.violation_labels[ppe_guard::PART_HELMET].clear();
    config.positive_labels[ppe_guard::PART_HELMET].push_back("helmet");

    std::vector<AlgoResult> frame;
    frame.push_back(person_box(22, cv::Rect(0, 2, 100, 200)));

    guard.update(frame, 0, config);
    guard.update(frame, 500, config);
    const Outcome outcome = guard.update(frame, 1200, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
}

/* 目标：人太小时（头部区域只有几像素）不做缺帽推断。 */
void test_helmet_absence_skips_small_person()
{
    Guard guard;
    Config config = base_config();
    config.helmet_infer_by_absence = true;
    config.violation_labels[ppe_guard::PART_HELMET].clear();
    config.positive_labels[ppe_guard::PART_HELMET].push_back("helmet");

    std::vector<AlgoResult> frame;
    frame.push_back(person_box(23, cv::Rect(0, 100, 40, 50)));

    guard.update(frame, 0, config);
    guard.update(frame, 500, config);
    const Outcome outcome = guard.update(frame, 1200, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(status_of(only_person(outcome), ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
}

/* 目标：两人挨着站、只有一个安全帽框能覆盖两人搜索区时，两人都不推断缺帽。 */
void test_helmet_shared_credit_between_neighbours()
{
    Guard guard;
    Config config = base_config();
    config.helmet_infer_by_absence = true;
    config.violation_labels[ppe_guard::PART_HELMET].clear();
    config.positive_labels[ppe_guard::PART_HELMET].push_back("helmet");

    std::vector<AlgoResult> frame;
    frame.push_back(person_box(31, cv::Rect(100, 120, 120, 300)));
    frame.push_back(person_box(32, cv::Rect(200, 120, 120, 300)));
    frame.push_back(violation_box("helmet", cv::Rect(170, 95, 40, 30)));

    guard.update(frame, 0, config);
    guard.update(frame, 500, config);
    const Outcome outcome = guard.update(frame, 1200, config);
    CHECK(outcome.candidate_observations == 0);
    CHECK(outcome.people.size() == 2);
    for (size_t i = 0; i < outcome.people.size(); ++i)
        CHECK(status_of(outcome.people[i], ppe_guard::PART_HELMET) == ppe_guard::PART_UNKNOWN);
}

} // namespace

int main()
{
    test_confirmation_needs_count_and_span();
    test_gap_dedupes_adjacent_frames();
    test_window_expiry();
    test_occlusion_marks_unknown();
    test_visible_parts_are_clear();
    test_partial_visibility();
    test_ambiguous_attribution_is_dropped();
    test_hand_side_resolution();
    test_ambiguous_hand_side();
    test_reacquire_starts_new_session();
    test_duplicate_track_in_one_frame();
    test_timestamp_regression();
    test_disabled_part();
    test_score_and_label_filter();
    test_state_ttl();
    test_helmet_geometry_rejects_low_box();
    test_helmet_geometry_rejects_big_box();
    test_helmet_geometry_accepts_top_box();
    test_workwear_band();
    test_glove_geometry_rejects_far_box();
    test_glove_requires_wrist();
    test_geometry_gate_can_be_disabled();
    test_helmet_geometry_keeps_correct_position_box();
    test_neutral_head_label_is_not_a_violation();
    test_dedupe_drops_overlapping_boxes();
    test_dedupe_keeps_distinct_boxes();
    test_low_confidence_never_counts();
    test_positive_veto_blocks_violation();
    test_require_pose_blocks_violation();
    test_require_pose_allows_visible_part();
    test_helmet_absence_infers_violation();
    test_helmet_absence_blocked_by_occluder();
    test_helmet_absence_off_by_default();
    test_helmet_absence_needs_confident_person();
    test_helmet_above_person_still_counts();
    test_helmet_absence_skips_edge_person();
    test_helmet_absence_skips_small_person();
    test_helmet_shared_credit_between_neighbours();

    if (g_failures == 0)
    {
        std::printf("ppe_guard: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("ppe_guard: %d/%d checks FAILED\n", g_failures, g_checks);
    return 1;
}
