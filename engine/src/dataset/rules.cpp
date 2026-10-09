#include "dataset/rules.h"
#include "cJSON.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <set>
#include <stdexcept>

namespace dataset {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
const cJSON* field(const cJSON* object, const char* name) { return cJSON_GetObjectItemCaseSensitive(object, name); }
std::string string_value(const cJSON* value, const char* name) {
    if (!cJSON_IsString(value) || !value->valuestring || !*value->valuestring)
        throw std::runtime_error(std::string("缺少有效的字段：") + name);
    return value->valuestring;
}
void keys(const cJSON* object, const std::set<std::string>& allowed) {
    if (!cJSON_IsObject(object)) throw std::runtime_error("条件必须是对象");
    std::set<std::string> seen;
    for (auto item = object->child; item; item = item->next)
        if (!item->string || !allowed.count(item->string) || !seen.insert(item->string).second)
            throw std::runtime_error("条件包含未知或重复字段");
}
Target parse_target(const cJSON* json, const std::vector<std::string>& labels,
                    const std::vector<std::string>& allowed, double floor, bool strict = true) {
    if (strict) keys(json, {"class", "min_confidence"});
    Target result;
    result.label = string_value(field(json, "class"), "class");
    if (std::find(labels.begin(), labels.end(), result.label) == labels.end())
        throw std::runtime_error("模型不支持类别：" + result.label);
    if (!allowed.empty() && std::find(allowed.begin(), allowed.end(), result.label) == allowed.end())
        throw std::runtime_error("模型的检测类别过滤未包含：" + result.label);
    auto confidence = field(json, "min_confidence");
    if (confidence) {
        if (!cJSON_IsNumber(confidence) || !std::isfinite(confidence->valuedouble)) throw std::runtime_error("置信度必须是数字");
        result.confidence = confidence->valuedouble;
    }
    if (result.confidence < 0 || result.confidence > 1 || result.confidence + 1e-6 < floor)
        throw std::runtime_error("条件置信度必须在模型输出阈值至 1 之间");
    return result;
}
std::string parse_op(const cJSON* json) {
    auto op = string_value(json, "op");
    if (!std::set<std::string>{"==", "!=", ">", ">=", "<", "<="}.count(op)) throw std::runtime_error("无效的数量比较符");
    return op;
}
void parse_count(const cJSON* count, Condition& result) {
    keys(count, {"op", "value"});
    result.op = parse_op(field(count, "op"));
    auto value = field(count, "value");
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 0 ||
        value->valuedouble > 10000 || std::floor(value->valuedouble) != value->valuedouble)
        throw std::runtime_error("数量必须是 0 至 10000 的整数");
    result.count = static_cast<int>(value->valuedouble);
}
double range_value(const cJSON* json, const char* name, double fallback, double maximum) {
    auto value = field(json, name);
    if (!value) return fallback;
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble <= 0 || value->valuedouble > maximum)
        throw std::runtime_error(std::string("匹配阈值超出有效范围：") + name);
    return value->valuedouble;
}
Condition parse_condition(const cJSON* json, int depth, int& nodes, const std::vector<std::string>& labels,
                           const std::vector<std::string>& allowed, double floor) {
    if (depth > 8 || ++nodes > 256) throw std::runtime_error("条件过于复杂：最多 8 层、256 个条件节点");
    keys(json, {"all", "any", "not", "class", "min_confidence", "count", "compare_counts", "association"});
    Condition result;
    for (const char* group : {"all", "any", "not"}) {
        auto child = field(json, group);
        if (!child) continue;
        if (cJSON_GetArraySize(json) != 1) throw std::runtime_error("条件组只能包含 all、any、not 中的一项");
        result.kind = std::string(group) == "all" ? Condition::All : std::string(group) == "any" ? Condition::Any : Condition::Not;
        if (result.kind == Condition::Not) result.children.push_back(parse_condition(child, depth + 1, nodes, labels, allowed, floor));
        else {
            if (!cJSON_IsArray(child) || !child->child) throw std::runtime_error("条件组不能为空");
            for (auto item = child->child; item; item = item->next)
                result.children.push_back(parse_condition(item, depth + 1, nodes, labels, allowed, floor));
        }
        return result;
    }
    if (auto compare = field(json, "compare_counts")) {
        if (cJSON_GetArraySize(json) != 1) throw std::runtime_error("数量比较不能混入其他条件字段");
        keys(compare, {"left", "right", "op"});
        result.kind = Condition::CompareCounts;
        result.subject = parse_target(field(compare, "left"), labels, allowed, floor);
        result.object = parse_target(field(compare, "right"), labels, allowed, floor);
        result.op = parse_op(field(compare, "op"));
        return result;
    }
    if (auto association = field(json, "association")) {
        if (cJSON_GetArraySize(json) != 1) throw std::runtime_error("关联判断不能混入其他条件字段");
        keys(association, {"subject", "object", "relation", "state", "quantifier", "count", "one_to_one", "min_overlap", "max_distance"});
        result.kind = Condition::Association;
        result.subject = parse_target(field(association, "subject"), labels, allowed, floor);
        result.object = parse_target(field(association, "object"), labels, allowed, floor);
        result.relation = string_value(field(association, "relation"), "relation");
        if (!std::set<std::string>{"center_inside", "overlap", "near"}.count(result.relation)) throw std::runtime_error("无效的空间匹配方式");
        result.state = string_value(field(association, "state"), "state");
        if (result.state != "matched" && result.state != "unmatched") throw std::runtime_error("无效的关联匹配状态");
        if (field(association, "quantifier")) result.quantifier = string_value(field(association, "quantifier"), "quantifier");
        if (!std::set<std::string>{"any", "all", "count"}.count(result.quantifier)) throw std::runtime_error("无效的关联目标数量要求");
        result.op = ">=";
        if (field(association, "count")) parse_count(field(association, "count"), result);
        else if (result.quantifier == "count") throw std::runtime_error("按数量判断关联时必须填写 count");
        if (auto one = field(association, "one_to_one")) {
            if (!cJSON_IsBool(one)) throw std::runtime_error("one_to_one 必须是布尔值");
            result.one_to_one = cJSON_IsTrue(one);
        }
        result.min_overlap = range_value(association, "min_overlap", 0.5, 1);
        result.max_distance = range_value(association, "max_distance", 1, 100);
        return result;
    }
    auto target = parse_target(json, labels, allowed, floor, false);
    result.label = target.label; result.confidence = target.confidence;
    parse_count(field(json, "count"), result);
    return result;
}
bool compare_count(int left, const std::string& op, int right) {
    if (op == "==") return left == right;
    if (op == "!=") return left != right;
    if (op == ">") return left > right;
    if (op == ">=") return left >= right;
    if (op == "<") return left < right;
    return left <= right;
}
bool valid_box(const Detection& d) {
    return std::isfinite(d.x) && std::isfinite(d.y) && std::isfinite(d.width) && std::isfinite(d.height) && d.width > 0 && d.height > 0;
}
bool related(const Detection& a, const Detection& b, const Condition& c) {
    const double bx = b.x + b.width / 2, by = b.y + b.height / 2;
    if (c.relation == "center_inside") return bx >= a.x && bx < a.x + a.width && by >= a.y && by < a.y + a.height;
    if (c.relation == "near") {
        const double dx = bx - a.x - a.width / 2, dy = by - a.y - a.height / 2;
        return dx * dx + dy * dy <= c.max_distance * c.max_distance * (a.width * a.width + a.height * a.height);
    }
    const double w = std::max(0.0, std::min(a.x + a.width, b.x + b.width) - std::max(a.x, b.x));
    const double h = std::max(0.0, std::min(a.y + a.height, b.y + b.height) - std::max(a.y, b.y));
    return w * h / (b.width * b.height) >= c.min_overlap;
}
int matching_count(const std::vector<Detection>& detections, const Condition& c, int& subjects) {
    std::vector<size_t> a, b;
    for (size_t i = 0; i < detections.size(); ++i) {
        const auto& d = detections[i];
        if (!valid_box(d)) continue;
        if (d.in_scope && d.label == c.subject.label && d.score >= c.subject.confidence) a.push_back(i);
        // ROI 只限制主体；关联目标允许来自同帧模型输出的其他位置。
        if (d.label == c.object.label && d.score >= c.object.confidence) b.push_back(i);
    }
    subjects = static_cast<int>(a.size());
    std::vector<std::vector<size_t>> edges(a.size());
    for (size_t i = 0; i < a.size(); ++i) for (size_t j = 0; j < b.size(); ++j)
        if (a[i] != b[j] && related(detections[a[i]], detections[b[j]], c)) edges[i].push_back(j);
    if (!c.one_to_one) return std::count_if(edges.begin(), edges.end(), [](const std::vector<size_t>& e) { return !e.empty(); });
    // 增广匹配避免贪心分配把可配齐的两个主体误判为缺少关联目标。
    std::vector<int> owner(b.size(), -1);
    std::function<bool(size_t, std::vector<bool>&)> assign = [&](size_t i, std::vector<bool>& visited) {
        for (size_t j : edges[i]) {
            if (visited[j]) continue;
            visited[j] = true;
            if (owner[j] < 0 || assign(static_cast<size_t>(owner[j]), visited)) { owner[j] = static_cast<int>(i); return true; }
        }
        return false;
    };
    int matched = 0;
    for (size_t i = 0; i < a.size(); ++i) { std::vector<bool> visited(b.size(), false); if (assign(i, visited)) ++matched; }
    return matched;
}
}
std::vector<Rule> parse_rules(const std::string& text, const std::vector<std::string>& labels,
                              const std::vector<std::string>& allowed, double floor) {
    Json json(cJSON_ParseWithOpts(text.c_str(), nullptr, 1), cJSON_Delete);
    if (!json || !cJSON_IsArray(json.get()) || !json->child || cJSON_GetArraySize(json.get()) > 32)
        throw std::runtime_error("采集规则必须是包含 1 至 32 条规则的数组");
    std::vector<Rule> result;
    std::set<std::string> ids;
    int nodes = 0;
    for (auto item = json->child; item; item = item->next) {
        keys(item, {"id", "name", "condition"});
        Rule rule;
        rule.id = string_value(field(item, "id"), "id");
        rule.name = string_value(field(item, "name"), "name");
        if (rule.id.size() > 128 || rule.name.size() > 256 || !ids.insert(rule.id).second)
            throw std::runtime_error("规则 ID 必须唯一，ID 和名称不能过长");
        rule.condition = parse_condition(field(item, "condition"), 0, nodes, labels, allowed, floor);
        result.push_back(std::move(rule));
    }
    return result;
}
bool Condition::matches(const std::vector<Detection>& detections) const {
    if (kind == Not) return !children.front().matches(detections);
    if (kind == All) return std::all_of(children.begin(), children.end(), [&](const Condition& c) { return c.matches(detections); });
    if (kind == Any) return std::any_of(children.begin(), children.end(), [&](const Condition& c) { return c.matches(detections); });
    if (kind == Association) {
        int subjects = 0;
        const int matched = matching_count(detections, *this, subjects);
        if (!subjects) return false;
        const int actual = state == "matched" ? matched : subjects - matched;
        if (quantifier == "all") return actual == subjects;
        if (quantifier == "any") return actual > 0;
        return compare_count(actual, op, count);
    }
    if (kind == CompareCounts) {
        int left = 0, right = 0;
        for (const auto& d : detections) if (d.in_scope) {
            if (d.label == subject.label && d.score >= subject.confidence) ++left;
            if (d.label == object.label && d.score >= object.confidence) ++right;
        }
        return compare_count(left, op, right);
    }
    int actual = 0;
    for (const auto& d : detections) if (d.in_scope && d.label == label && d.score >= confidence) ++actual;
    return compare_count(actual, op, count);
}
void Gate::reset() {
    std::fill(since_.begin(), since_.end(), 0);
    std::fill(previous_.begin(), previous_.end(), false);
    frame_ = -1; time_ = 0;
    // 断流或关闭推理不能绕过已经使用的采样间隔。
}
std::vector<size_t> Gate::update(int64_t frame, uint64_t now, const std::vector<bool>& matches,
                               uint64_t confirm_ms, uint64_t interval_ms, uint64_t gap_ms, bool on_enter) {
    if (frame == frame_) return {};
    if (frame < frame_ || now < time_ || (time_ && now - time_ > gap_ms)) reset();
    frame_ = frame; time_ = now;
    std::vector<size_t> ready;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (!matches[i]) { since_[i] = 0; previous_[i] = false; continue; }
        if (!since_[i]) since_[i] = now;
        bool confirmed = now - since_[i] >= confirm_ms;
        if (confirmed && (!on_enter || !previous_[i])) ready.push_back(i);
    }
    if (ready.empty() || (attempted_ && now >= attempt_ && now - attempt_ < interval_ms)) return {};
    attempted_ = true; attempt_ = now;
    for (size_t i : ready) previous_[i] = true;
    return ready;
}
}
