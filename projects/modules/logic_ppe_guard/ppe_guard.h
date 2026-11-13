#pragma once

/* 安全穿戴的纯逻辑内核：只依赖检测结果，不接触 ChannelContext。
 * 这样部位状态机、证据窗口和归属判定可以被 engine/tests/ppe 直接实例化验证。 */

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <types.h>

namespace ppe_guard
{

enum PartIndex
{
    PART_HELMET = 0,
    PART_WORKWEAR,
    PART_HAND_LEFT,
    PART_HAND_RIGHT,
    PART_COUNT
};

enum PartStatus
{
    PART_UNKNOWN = 0, /* 遮挡、模糊、无姿态或归属不明：不可核验 */
    PART_CLEAR,       /* 本帧部位可核验且未见违规证据；不等于“装备符合要求” */
    PART_CANDIDATE,   /* 已有违规证据，未达确认门槛 */
    PART_VIOLATION    /* 已达确认门槛 */
};

struct Config
{
    std::vector<std::string> person_labels;
    std::array<std::vector<std::string>, PART_COUNT> violation_labels;
    std::array<bool, PART_COUNT> enabled = {{true, true, true, true}};
    float person_min_score = 0.35f;
    float violation_min_score = 0.75f;
    float keypoint_min_score = 0.3f;
    uint64_t evidence_window_ms = 4000;
    uint64_t evidence_gap_ms = 250;
    size_t required_observations = 3;
    uint64_t required_span_ms = 1000;
    uint64_t reacquire_gap_ms = 3000;
    uint64_t state_ttl_ms = 600000;

    /* 部位几何门控：类别说“这是安全帽”，但框长在腰上时不该算证据。
     * 多帧确认只能挡随机噪声，挡不住这种系统性的“类别对了、位置错了”。 */
    bool geometry_gate = true;
    float helmet_top_ratio = 0.35f;      /* 安全帽框中心允许的最高位置（占人员框高） */
    float helmet_area_min_ratio = 0.01f; /* 安全帽框 / 人员框 面积比下限 */
    float helmet_area_max_ratio = 0.12f; /* 面积比上限 */
    float helmet_head_ratio = 0.35f;
    float helmet_search_up_ratio = 0.6f; /* 安全帽框允许高出人体框顶的比例 */
    float helmet_search_side_ratio = 0.15f;
    /* 安全帽框允许左右外扩的比例 */     /* 框中心到鼻/眼关键点的距离上限（占人员框高） */
    float workwear_top_ratio = 0.25f;    /* 工作服框中心允许的垂直区间起点 */
    float workwear_bottom_ratio = 0.75f; /* 区间终点 */
    float glove_wrist_ratio = 0.2f;      /* 手套框中心到对应腕点的距离上限（占人员框高） */

    /* 同一物品常被输出多个重叠框：同类别且 IoU 超阈值时只留置信度最高的一个。 */
    float dedupe_iou = 0.6f;
    /* 只有该部位关键点可见（没被遮挡、没出画）时才采信违规证据。 */
    bool require_pose_for_violation = false;
    /* 正类否决：同一部位上出现“穿好了”的正类框时，负类框不再作数。 */
    bool positive_veto = true;
    /* 正类缺失推断：模型只有“戴好了”的正类（如 helmet）时，用“头部区域里连续没有
     * 安全帽框”推断未戴。只有头部可核验（没被别人挡住、没贴边）时才计数。 */
    bool helmet_infer_by_absence = false;
    std::array<std::vector<std::string>, PART_COUNT> positive_labels;
};

/* 单个部位的近期证据。遮挡帧不写证据也不清零，等窗口自然过期。 */
struct PartEvidence
{
    std::deque<uint64_t> violations;
    bool confirmed = false;
};

struct PersonState
{
    std::array<PartEvidence, PART_COUNT> parts;
    PartEvidence hand_unsided; /* 违规框分不清左右手时的兜底证据 */
    uint64_t last_seen_ms = 0;
    bool reported = false;
};

struct Evaluation
{
    int track_id = -1;
    cv::Rect box;
    float score = 0.0f;
    std::array<int, PART_COUNT> status = {{0, 0, 0, 0}};
    bool gloves_unsided = false; /* 已确认的手套违规，但分不清左右手 */
    bool violation = false;
    bool report_due = false; /* 已确认且本会话尚未上报 */
    std::string violation_types;
    std::string part_states;
};

struct Outcome
{
    std::vector<Evaluation> people;
    int candidate_observations = 0;
    int rejected_observations = 0;  /* 类别匹配但被归属/几何门控丢弃的违规框数 */
    int duplicate_observations = 0; /* 同类重叠框被去重丢弃的数量 */
    int vetoed_observations = 0;    /* 因同部位已有正类框而不采信的负类框数量 */
};

/* 逗号分隔的标签串切成标签数组，去空格、丢空项。 */
std::vector<std::string> split_labels(const std::string &text);
bool matches_label(const std::string &label, const std::vector<std::string> &labels);

const char *part_full_name(int part);
char status_char(int status);

class Guard
{
  public:
    Outcome update(const std::vector<AlgoResult> &results, uint64_t now_ms, const Config &config);
    /* 事件被接受后调用，避免同一会话重复上报。 */
    void mark_reported(int track_id);
    size_t tracked_people() const
    {
        return people_.size();
    }
    void reset()
    {
        people_.clear();
        clock_initialised_ = false;
    }

  private:
    std::unordered_map<int, PersonState> people_;
    bool clock_initialised_ = false;
    uint64_t last_ms_ = 0;
};

} // namespace ppe_guard
