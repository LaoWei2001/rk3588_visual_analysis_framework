/** @file dataset.h 独立的数据集条件匹配与时间门控，与视频采集共用。 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dataset
{
struct Detection
{
    std::string label;
    double score, x, y, width, height;
    bool in_scope;
    /** @brief 构造规则检测输入；位置/宽高在同一坐标系，scope 表示是否属于采集主体范围。
     * @note 几何关联要求有限坐标和正宽高；scope=false 的框仍可作为关联目标。
     * 示例：
     * @code
     * dataset::Detection person("person", 0.9, 10, 20, 100, 200, true);
     * @endcode
     */
    Detection(const std::string &name, double confidence, double left = 0, double top = 0, double w = 0, double h = 0,
              bool scope = true)
        : label(name), score(confidence), x(left), y(top), width(w), height(h), in_scope(scope)
    {
    }
};
struct Target
{
    std::string label;
    double confidence = 0.3;
};
struct Condition
{
    enum Kind
    {
        Leaf,
        All,
        Any,
        Not,
        CompareCounts,
        Association
    } kind = Leaf;
    std::string label, op;
    double confidence = 0.3;
    int count = 1;
    std::string object_op = ">";
    int object_count = 1;
    Target subject, object;
    // 关联类别按“任意一类”合并匹配；object 仍用于两类数量比较。
    std::vector<Target> objects;
    std::string relation = "center_inside", state = "unmatched", quantifier = "any";
    double min_overlap = 0.5, max_distance = 1;
    bool one_to_one = true;
    std::vector<Condition> children;
    /** @brief 对同帧检测求条件真假；建议由 parse_rules() 构造合法条件。
     * @note 组合条件必须非空，Not 恰有一个子条件；否则抛 invalid_argument。
     * 示例：
     * @code
     * dataset::Condition c;
     * c.label = "person";
     * c.op = ">=";
     * bool match = c.matches({dataset::Detection("person", 0.9)});
     * @endcode
     */
    bool matches(const std::vector<Detection> &detections) const;
};
struct Rule
{
    std::string id, name;
    Condition condition;
};
/** @brief 解析并校验 1~32 条 JSON 采集规则，返回自有数据；错误抛 runtime_error。
 * @param labels 模型类别；allowed 为允许类别（空表示不限）；output_threshold 为引擎输出最低门槛。
 * 示例：
 * @code
 * auto rules = dataset::parse_rules(rules_json, {"person"}, {}, 0.3);
 * @endcode
 */
std::vector<Rule> parse_rules(const std::string &json, const std::vector<std::string> &labels,
                              const std::vector<std::string> &allowed, double output_threshold);
// 每条规则单独确认；全通道限制采样间隔，同一来源帧只评估一次。
class Gate
{
  public:
    /** @brief 创建固定规则数的确认/采样门控器；每个采集任务独立持有。
     * 示例：
     * @code
     * dataset::Gate gate(2);
     * @endcode
     */
    explicit Gate(size_t rules) : since_(rules, 0), matching_(rules, false), previous_(rules, false)
    {
    }
    /** @brief 按非负来源帧号和同一毫秒时钟更新规则；返回允许采样的规则下标。
     * @param matches 长度必须等于构造规则数；confirm_ms/interval_ms/gap_ms 为确认/最小采样间隔/断流阈值。
     * @note on_enter 仅首次确认触发；同 frame 不重复处理，长度不符抛 invalid_argument。
     * 示例：
     * @code
     * dataset::Gate gate(1);
     * auto fired = gate.update(0, 0, {true}, 0, 1000, 2000, true);
     * @endcode
     */
    std::vector<size_t> update(int64_t frame, uint64_t now, const std::vector<bool> &matches, uint64_t confirm_ms,
                               uint64_t interval_ms, uint64_t gap_ms, bool on_enter);
    /** @brief 清除确认、进入状态和当前帧记录，但保留已消耗的最小采样间隔。
     * 示例：
     * @code
     * gate.reset(); // 完全重新开始一个任务时请新建 Gate
     * @endcode
     */
    void reset();

  private:
    int64_t frame_ = -1;
    uint64_t time_ = 0, attempt_ = 0;
    bool attempted_ = false;
    std::vector<uint64_t> since_;
    std::vector<bool> matching_, previous_;
};
} // namespace dataset
