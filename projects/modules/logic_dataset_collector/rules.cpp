#include "rules.h"
#include "cJSON.h"
#include <algorithm>
#include <cmath>
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
Condition parse_condition(const cJSON* json, int depth, int& nodes, const std::vector<std::string>& labels,
                           const std::vector<std::string>& allowed, double floor) {
    if (depth > 8 || ++nodes > 256) throw std::runtime_error("条件过于复杂：最多 8 层、256 个条件节点");
    keys(json, {"all", "any", "not", "class", "min_confidence", "count"});
    Condition result;
    for (const char* group : {"all", "any", "not"}) {
        auto child = field(json, group);
        if (!child) continue;
        if (cJSON_GetArraySize(json) != 1) throw std::runtime_error("条件组只能包含 all、any、not 中的一项");
        result.kind = std::string(group) == "all" ? Condition::All : std::string(group) == "any" ? Condition::Any : Condition::Not;
        if (result.kind == Condition::Not) {
            result.children.push_back(parse_condition(child, depth + 1, nodes, labels, allowed, floor));
        } else {
            if (!cJSON_IsArray(child) || !child->child) throw std::runtime_error("条件组不能为空");
            for (auto item = child->child; item; item = item->next)
                result.children.push_back(parse_condition(item, depth + 1, nodes, labels, allowed, floor));
        }
        return result;
    }
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
    auto count = field(json, "count");
    keys(count, {"op", "value"});
    result.op = string_value(field(count, "op"), "count.op");
    if (!std::set<std::string>{"==", "!=", ">", ">=", "<", "<="}.count(result.op)) throw std::runtime_error("无效的数量比较符");
    auto value = field(count, "value");
    if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) || value->valuedouble < 0 ||
        value->valuedouble > 10000 || std::floor(value->valuedouble) != value->valuedouble)
        throw std::runtime_error("数量必须是 0 至 10000 的整数");
    result.count = static_cast<int>(value->valuedouble);
    return result;
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
    int actual = 0;
    for (const auto& d : detections) if (d.label == label && d.score >= confidence) ++actual;
    if (op == "==") return actual == count;
    if (op == "!=") return actual != count;
    if (op == ">") return actual > count;
    if (op == ">=") return actual >= count;
    if (op == "<") return actual < count;
    return actual <= count;
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
