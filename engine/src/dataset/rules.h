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
    Target subject, object;
    std::string relation = "center_inside", state = "unmatched", quantifier = "any";
    double min_overlap = 0.5, max_distance = 1;
    bool one_to_one = true;
    std::vector<Condition> children;
    bool matches(const std::vector<Detection> &detections) const;
};
struct Rule
{
    std::string id, name;
    Condition condition;
};
std::vector<Rule> parse_rules(const std::string &json, const std::vector<std::string> &labels,
                              const std::vector<std::string> &allowed, double output_threshold);
// 每条规则单独确认；全通道限制采样间隔，同一来源帧只评估一次。
class Gate
{
  public:
    explicit Gate(size_t rules) : since_(rules, 0), previous_(rules, false)
    {
    }
    std::vector<size_t> update(int64_t frame, uint64_t now, const std::vector<bool> &matches, uint64_t confirm_ms,
                               uint64_t interval_ms, uint64_t gap_ms, bool on_enter);
    void reset();

  private:
    int64_t frame_ = -1;
    uint64_t time_ = 0, attempt_ = 0;
    bool attempted_ = false;
    std::vector<uint64_t> since_;
    std::vector<bool> previous_;
};
} // namespace dataset
