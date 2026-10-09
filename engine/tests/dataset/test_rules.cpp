#include "dataset/rules.h"
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <chrono>
using namespace dataset;
std::string rule(const std::string& condition) { return "[{\"id\":\"r\",\"name\":\"r\",\"condition\":" + condition + "}]"; }
std::string absent(const std::string& c) { return "{\"class\":\"" + c + "\",\"min_confidence\":0.3,\"count\":{\"op\":\"==\",\"value\":0}}"; }
bool rejected(const std::string& text, std::vector<std::string> allowed = {}, double threshold = 0.3) {
    try { parse_rules(text, {"x", "y"}, allowed, threshold); return false; } catch (const std::runtime_error&) { return true; }
}
int main() {
    auto association_json = [](const std::string& extra = "") {
        return "{\"association\":{\"subject\":{\"class\":\"x\",\"min_confidence\":0.3},"
               "\"object\":{\"class\":\"y\",\"min_confidence\":0.5},\"relation\":\"center_inside\",\"state\":\"unmatched\"" + extra + "}}";
    };
    auto linked = parse_rules(rule(association_json()), {"x", "y"}, {}, 0.3)[0].condition;
    const Detection a("x", 0.9, 0, 0, 100, 200), b("x", 0.9, 200, 0, 100, 200);
    const Detection hat_a("y", 0.9, 30, 10, 20, 20), hat_b("y", 0.9, 230, 10, 20, 20);
    assert(linked.matches({a, b, hat_a})); // 两个主体只匹配到一个关联目标。
    assert(!linked.matches({a, b, hat_a, hat_b}));
    assert(linked.matches({a, b, hat_a, Detection("y", 0.9, 900, 0, 20, 20)})); // 数量相等但位置不对应。
    assert(!linked.matches({})); // 不把没有主体解释为“全部没有匹配到”。
    assert(!linked.matches({hat_a}));
    assert(linked.matches({a, Detection("y", 0.4, 30, 10, 20, 20)}));
    assert(!linked.matches({Detection("x", 0.2, 0, 0, 100, 200)}));
    assert(!linked.matches({Detection("x", 0.9, 0, 0, 0, 200)}));
    auto all_missing = parse_rules(rule(association_json(",\"quantifier\":\"all\"")), {"x", "y"}, {}, 0.3)[0].condition;
    assert(!all_missing.matches({a, b, hat_a}));
    assert(all_missing.matches({a, b}));
    assert(!all_missing.matches({}));
    auto two_missing = parse_rules(rule(association_json(",\"quantifier\":\"count\",\"count\":{\"op\":\">=\",\"value\":2}")), {"x", "y"}, {}, 0.3)[0].condition;
    assert(two_missing.matches({a, b}));
    assert(!two_missing.matches({a, b, hat_a}));
    // 同一关联目标不能同时分配给两个主体；可显式允许复用。
    assert(linked.matches({a, a, hat_a}));
    auto shared = parse_rules(rule(association_json(",\"one_to_one\":false")), {"x", "y"}, {}, 0.3)[0].condition;
    assert(!shared.matches({a, a, hat_a}));
    // 增广路径：第一个主体可匹配两个目标，第二个只能匹配第一个。
    assert(!linked.matches({Detection("x", 0.9, 0, 0, 20, 20), Detection("x", 0.9, 8, 8, 4, 4),
                            Detection("y", 0.9, 9, 9, 2, 2), Detection("y", 0.9, 1, 1, 2, 2)}));
    // ROI 限制主体，主体框内的关联目标可以在计数 ROI 外。
    assert(!linked.matches({a, Detection("x", 0.9, 200, 0, 100, 200, false),
                            Detection("y", 0.9, 30, 10, 20, 20, false)}));
    assert(!linked.matches({Detection("x", 0.9, 0, 0, 100, 200, false), hat_a}));
    auto overlap = linked; overlap.relation = "overlap";
    assert(!overlap.matches({a, Detection("y", 0.9, 90, 10, 20, 20)})); // 关联目标的一半面积在主体框内。
    assert(overlap.matches({a, Detection("y", 0.9, 91, 10, 20, 20)}));
    auto nearby = linked; nearby.relation = "near"; nearby.max_distance = 0.1;
    assert(!nearby.matches({a, Detection("y", 0.9, 45, 95, 10, 10)}));
    assert(nearby.matches({a, hat_a}));
    auto matched = linked; matched.state = "matched";
    assert(matched.matches({a, b, hat_a}));
    matched.quantifier = "all";
    assert(!matched.matches({a, b, hat_a}));
    assert(matched.matches({a, b, hat_a, hat_b}));
    // 同类关联不允许自己匹配自己。
    auto same = linked; same.object = same.subject;
    assert(same.matches({a}));
    assert(!same.matches({a, a}));
    const std::string comparison_json = "{\"compare_counts\":{\"left\":{\"class\":\"x\",\"min_confidence\":0.3},\"right\":{\"class\":\"y\",\"min_confidence\":0.5},\"op\":\">\"}}";
    auto compare = parse_rules(rule(comparison_json), {"x", "y"}, {}, 0.3)[0].condition;
    assert(compare.matches({a, b, hat_a}));
    assert(!compare.matches({a, b, hat_a, hat_b}));
    assert(compare.matches({a, Detection("y", 0.9, 30, 10, 20, 20, false)}));
    assert(!compare.matches({Detection("x", 0.9, 0, 0, 100, 200, false)}));
    auto combined = parse_rules(rule("{\"all\":[" + association_json() + "," + comparison_json + "]}"), {"x", "y"}, {}, 0.3)[0].condition;
    assert(combined.matches({a, b, hat_a}));
    assert(!combined.matches({a, b, hat_a, hat_b}));
    assert(rejected(rule(association_json(",\"one_to_one\":1"))));
    assert(rejected(rule(association_json(",\"quantifier\":\"count\""))));
    assert(rejected(rule(association_json(",\"min_overlap\":0"))));
    assert(rejected(rule(association_json(",\"max_distance\":-1"))));
    assert(rejected(rule(association_json(",\"bad\":true"))));
    assert(rejected(rule(association_json()), {"x"}));
    assert(rejected(rule(association_json()), {}, 0.4));
    assert(rejected(rule("{\"association\":{},\"class\":\"x\"}")));
    assert(rejected(rule("{\"compare_counts\":{\"left\":{\"class\":\"unknown\"},\"right\":{\"class\":\"y\"},\"op\":\">\"}}")));
    std::vector<Detection> many;
    for (int i = 0; i < 64; ++i) {
        many.emplace_back("x", 0.9, i * 100, 0, 90, 200);
        many.emplace_back("y", 0.9, i * 100 + 20, 10, 20, 20);
    }
    auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) assert(!linked.matches(many));
    std::cout << "64-pair association average us: " << std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / 1000 << '\n';
    const auto any = parse_rules(rule("{\"any\":[" + absent("x") + "," + absent("y") + "]}"), {"x", "y"}, {}, 0.3);
    const auto all = parse_rules(rule("{\"all\":[" + absent("x") + "," + absent("y") + "]}"), {"x", "y"}, {}, 0.3);
    for (int x = 0; x < 2; ++x) for (int y = 0; y < 2; ++y) {
        std::vector<Detection> ds; if (x) ds.push_back({"x", 0.8}); if (y) ds.push_back({"y", 0.8});
        assert(any[0].condition.matches(ds) == (!x || !y));
        assert(all[0].condition.matches(ds) == (!x && !y));
    }
    auto negated = parse_rules(rule("{\"not\":" + absent("x") + "}"), {"x"}, {}, 0.3);
    assert(!negated[0].condition.matches({{"x", 0.2}}));
    assert(negated[0].condition.matches({{"x", 0.9}}));
    auto count = parse_rules(rule("{\"class\":\"x\",\"min_confidence\":0.5,\"count\":{\"op\":\">=\",\"value\":2}}"), {"x"}, {}, 0.3);
    assert(!count[0].condition.matches({{"x", 0.8}, {"x", 0.4}}));
    assert(count[0].condition.matches({{"x", 0.8}, {"x", 0.6}}));
    assert(rejected(rule(absent("unknown"))));
    assert(rejected(rule(absent("x")), {"y"}));
    assert(rejected(rule(absent("x")), {}, 0.5));
    assert(rejected(rule("{\"any\":[]}")));
    assert(rejected(rule("{\"all\":[" + absent("x") + "],\"not\":" + absent("x") + "}")));
    assert(rejected(rule("{\"class\":\"x\",\"count\":{\"op\":\"==\",\"value\":-1}}")));
    assert(rejected(rule(absent("x")) + " garbage"));
    std::string deep = absent("x"); for (int i = 0; i < 10; ++i) deep = "{\"not\":" + deep + "}";
    assert(rejected(rule(deep)));
    // ALL 必须完整验证，不能因为第一项肯定满足而接受未知类别。
    assert(rejected(rule("{\"all\":[" + absent("x") + "," + absent("unknown") + "]}")));
    Gate gate(2);
    assert(gate.update(1, 1000, {true, false}, 200, 1000, 2000, false).empty());
    assert(gate.update(1, 1300, {true, false}, 200, 1000, 2000, false).empty()); // duplicate
    assert(gate.update(2, 1200, {true, false}, 200, 1000, 2000, false) == std::vector<size_t>{0});
    assert(gate.update(3, 2000, {true, true}, 200, 1000, 2000, false).empty());
    assert(gate.update(4, 2200, {true, true}, 200, 1000, 2000, false) == (std::vector<size_t>{0, 1}));
    assert(gate.update(5, 5000, {true, true}, 200, 1000, 2000, false).empty()); // gap resets confirm
    Gate enter(1);
    assert(!enter.update(1, 1000, {true}, 0, 1000, 2000, true).empty());
    assert(enter.update(2, 1200, {true}, 0, 1000, 2000, true).empty());
    assert(enter.update(3, 1400, {false}, 0, 1000, 2000, true).empty());
    assert(enter.update(4, 1500, {true}, 0, 1000, 2000, true).empty());
    assert(!enter.update(5, 2000, {true}, 0, 1000, 2000, true).empty()); // entry retained during cooldown
    std::cout << "Dataset rules, matching and timing tests passed\n";
}
