#include "dataset/rules.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
using namespace dataset;
std::string rule(const std::string &condition)
{
    return "[{\"id\":\"r\",\"name\":\"r\",\"condition\":" + condition + "}]";
}
std::string absent(const std::string &c)
{
    return "{\"class\":\"" + c + "\",\"min_confidence\":0.3,\"count\":{\"op\":\"==\",\"value\":0}}";
}
bool rejected(const std::string &text, std::vector<std::string> allowed = {}, double threshold = 0.3)
{
    try
    {
        parse_rules(text, {"x", "y", "z"}, allowed, threshold);
        return false;
    }
    catch (const std::runtime_error &)
    {
        return true;
    }
}
int main()
{
    auto association_json = [](const std::string &extra = "") {
        return "{\"association\":{\"subject\":{\"class\":\"x\",\"min_confidence\":0.3},"
               "\"object\":{\"class\":\"y\",\"min_confidence\":0.5},\"relation\":\"center_inside\",\"state\":"
               "\"unmatched\"" +
               extra + "}}";
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
    auto two_missing =
        parse_rules(rule(association_json(",\"quantifier\":\"count\",\"count\":{\"op\":\">=\",\"value\":2}")),
                    {"x", "y"}, {}, 0.3)[0]
            .condition;
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
    assert(
        !linked.matches({a, Detection("x", 0.9, 200, 0, 100, 200, false), Detection("y", 0.9, 30, 10, 20, 20, false)}));
    assert(!linked.matches({Detection("x", 0.9, 0, 0, 100, 200, false), hat_a}));
    auto overlap = linked;
    overlap.relation = "overlap";
    assert(!overlap.matches({a, Detection("y", 0.9, 90, 10, 20, 20)})); // 关联目标的一半面积在主体框内。
    assert(overlap.matches({a, Detection("y", 0.9, 91, 10, 20, 20)}));
    // 逐顶帽子查找对应的人：包含方向和面积分母必须随关系一起反转。
    auto reverse_json = [](const std::string &relation) {
        return R"({"association":{"subject":{"class":"y","min_confidence":0.3},"object":{"class":"x","min_confidence":0.5},"relation":")" +
               relation + R"(","state":"unmatched","min_overlap":0.5,"one_to_one":false}})";
    };
    for (const auto &relation : {"subject_center_inside", "subject_overlap"})
    {
        auto reverse = parse_rules(rule(reverse_json(relation)), {"x", "y"}, {}, .3)[0].condition;
        assert(!reverse.matches({a, hat_a})); // 正常戴帽，不采集。
        assert(!reverse.matches({a, b, hat_a, hat_b}));
        assert(reverse.matches({a, hat_a, hat_b})); // 第二人的帽子在，但对应人框漏检。
        assert(reverse.matches({hat_a})); // 所有人框都漏检。
        assert(!reverse.matches({a})); // 无帽子主体，不触发。
        assert(!reverse.matches({a, Detection("y", .29, 30, 10, 20, 20)}));
        assert(reverse.matches({Detection("x", .49, 0, 0, 100, 200), hat_a}));
        assert(!reverse.matches({Detection("x", .9, 0, 0, 100, 200, false), hat_a}));
        assert(!reverse.matches({a, Detection("y", .9, 230, 10, 20, 20, false)}));
        assert(reverse.matches({Detection("x", .9, 0, 0, 0, 200), hat_a}));
        assert(!reverse.matches({a, hat_a, hat_a})); // 允许多顶帽子共享一个人框。
        auto assigned = reverse;
        assigned.one_to_one = true;
        assert(assigned.matches({a, hat_a, hat_a})); // 一对一分配不足，也会判为缺失。
        auto counted = reverse;
        counted.state = "count";
        counted.object_op = "==";
        counted.object_count = 0;
        assert(counted.matches({a, hat_a, hat_b}));
        assert(!counted.matches({a, b, hat_a, hat_b}));
    }
    auto reverse_area = parse_rules(rule(reverse_json("subject_overlap")), {"x", "y"}, {}, .3)[0].condition;
    assert(!reverse_area.matches({a, Detection("y", .9, 90, 10, 20, 20)})); // 恰好 50%，匹配。
    assert(reverse_area.matches({a, Detection("y", .9, 91, 10, 20, 20)}));
    auto reverse_center = parse_rules(rule(reverse_json("subject_center_inside")), {"x", "y"}, {}, .3)[0].condition;
    assert(reverse_center.matches({a, Detection("y", .9, 90, 10, 20, 20)})); // 中心恰好在右边界外。
    auto wrong_direction = parse_rules(rule(reverse_json("overlap")), {"x", "y"}, {}, .3)[0].condition;
    assert(wrong_direction.matches({a, hat_a})); // 原有关系仍保持原来按人框面积计算的含义。
    auto nearby = linked;
    nearby.relation = "near";
    nearby.max_distance = 0.1;
    assert(!nearby.matches({a, Detection("y", 0.9, 45, 95, 10, 10)}));
    assert(nearby.matches({a, hat_a}));
    auto matched = linked;
    matched.state = "matched";
    assert(matched.matches({a, b, hat_a}));
    matched.quantifier = "all";
    assert(!matched.matches({a, b, hat_a}));
    assert(matched.matches({a, b, hat_a, hat_b}));
    // 同类关联不允许自己匹配自己。
    auto same = linked;
    same.objects[0] = same.subject;
    assert(same.matches({a}));
    assert(!same.matches({a, a}));
    // 多类关联必须针对同一个主体判断：任意一类匹配成功就不算缺失。
    const auto multi_json = [](const std::string &objects, const std::string &extra = "") {
        return "{\"association\":{\"subject\":{\"class\":\"x\",\"min_confidence\":0.3},\"objects\":" + objects +
               ",\"relation\":\"center_inside\",\"state\":\"unmatched\"" + extra + "}}";
    };
    const std::string targets = R"([{"class":"y","min_confidence":0.5},{"class":"z","min_confidence":0.6}])";
    auto multi = parse_rules(rule(multi_json(targets)), {"x", "y", "z"}, {}, .3)[0].condition;
    const Detection head_a("z", .9, 30, 10, 20, 20), head_b("z", .9, 230, 10, 20, 20);
    assert(multi.matches({a, b, hat_a}));
    assert(multi.matches({a, b, head_a}));
    assert(!multi.matches({a, b, hat_a, head_b}));
    assert(!multi.matches({a, b, head_a, hat_b}));
    assert(!multi.matches({a, b, head_a, head_b}));
    assert(multi.matches({a, b, hat_a, head_a})); // 两类都在第一个人身上，第二个人仍然缺失。
    assert(multi.matches({a, b, hat_a, Detection("z", .59, 230, 10, 20, 20)}));
    assert(!multi.matches({a, b, hat_a, Detection("z", .6, 230, 10, 20, 20)}));
    assert(multi.matches({a, b, hat_a, Detection("z", .9, 900, 0, 20, 20)}));
    assert(!multi.matches({}));
    assert(!multi.matches({a, Detection("x", .9, 200, 0, 100, 200, false), hat_a}));
    assert(!multi.matches({a, Detection("z", .9, 30, 10, 20, 20, false)}));
    assert(multi.matches({a, a, head_a})); // 跨类别共用的一对一匹配仍然不能复用同一个检测框。
    auto multi_shared = multi;
    multi_shared.one_to_one = false;
    assert(!multi_shared.matches({a, a, head_a}));
    assert(!multi.matches({Detection("x", .9, 0, 0, 20, 20), Detection("x", .9, 8, 8, 4, 4),
                           Detection("y", .9, 9, 9, 2, 2), Detection("z", .9, 1, 1, 2, 2)}));
    auto multi_all = multi;
    multi_all.quantifier = "all";
    assert(multi_all.matches({a, b}));
    assert(!multi_all.matches({a, b, hat_a}));
    auto multi_count = multi;
    multi_count.quantifier = "count";
    multi_count.op = "==";
    multi_count.count = 1;
    assert(multi_count.matches({a, b, head_b}));
    assert(!multi_count.matches({a, b, hat_a, head_b}));
    multi_all.state = "matched";
    assert(multi_all.matches({a, b, hat_a, head_b}));
    assert(!multi_all.matches({a, b, head_a}));
    auto single_array = parse_rules(rule(multi_json(R"([{"class":"y","min_confidence":0.5}])")),
                                    {"x", "y"}, {}, .3)[0].condition;
    assert(single_array.matches({a, b, hat_a}) == linked.matches({a, b, hat_a}));
    assert(rejected(rule(multi_json("[]"))));
    assert(rejected(rule(multi_json("null"))));
    assert(rejected(rule(multi_json(R"({"class":"y"})"))));
    assert(rejected(rule(multi_json(R"([null])"))));
    assert(rejected(rule(multi_json(R"([{"class":"y"},{"class":"y"}])"))));
    assert(rejected(rule(multi_json(R"([{"class":"unknown"}])"))));
    assert(rejected(rule(multi_json(targets)), {"x", "y"}));
    assert(rejected(rule(multi_json(R"([{"class":"y","min_confidence":0.2}])"))));
    assert(rejected(rule(multi_json(targets, R"(,"object":{"class":"y"})"))));
    assert(rejected(rule(R"({"association":{"subject":{"class":"x"},"relation":"near","state":"matched"}})")));
    std::string too_many = "[";
    for (int i = 0; i < 33; ++i)
        too_many += (i ? "," : "") + std::string(R"({"class":"y"})");
    assert(rejected(rule(multi_json(too_many + "]"))));
    const auto count_json = [](const std::string &criteria = R"({"op":">","value":1})", const std::string &extra = "") {
        return R"({"association":{"subject":{"class":"x","min_confidence":0.3},"object":{"class":"y","min_confidence":0.5},"relation":"center_inside","state":"count","object_count":)" + criteria + extra + "}}";
    };
    auto excess = parse_rules(rule(count_json()), {"x", "y"}, {}, .3)[0].condition;
    const Detection pants_a("y", .9, 30, 150, 20, 20), pants_b("y", .9, 230, 150, 20, 20);
    assert(excess.matches({a, hat_a, pants_a})); // 头部真帽 + 裤子误报。
    assert(excess.matches({a, b, hat_a, pants_a})); // 总人数与帽子数相等，也能找出同一个人的异常。
    assert(!excess.matches({a, b, hat_a, hat_b}));
    assert(!excess.matches({a, hat_a, Detection("y", .9, 900, 150, 20, 20)}));
    assert(!excess.matches({a, hat_a, Detection("y", .49, 30, 150, 20, 20)}));
    assert(!excess.matches({a, hat_a, Detection("y", .9, 30, 150, 0, 20)}));
    assert(!excess.matches({}));
    assert(!excess.matches({hat_a, pants_a}));
    assert(!excess.matches({Detection("x", .9, 0, 0, 100, 200, false), hat_a, pants_a}));
    assert(excess.matches({a, hat_a, Detection("y", .9, 30, 150, 20, 20, false)}));
    auto all_excess = excess;
    all_excess.quantifier = "all";
    assert(!all_excess.matches({a, b, hat_a, pants_a, hat_b}));
    assert(all_excess.matches({a, b, hat_a, pants_a, hat_b, pants_b}));
    assert(!all_excess.matches({}));
    auto two_excess = parse_rules(rule(count_json(R"({"op":">","value":1})",
        R"(,"quantifier":"count","count":{"op":">=","value":2})")), {"x", "y"}, {}, .3)[0].condition;
    assert(two_excess.matches({a, b, hat_a, pants_a, hat_b, pants_b}));
    assert(!two_excess.matches({a, b, hat_a, pants_a, hat_b}));
    auto missing_count = parse_rules(rule(count_json(R"({"op":"==","value":0})")), {"x", "y"}, {}, .3)[0].condition;
    assert(missing_count.matches({a, b, hat_a}));
    assert(!missing_count.matches({a, b, hat_a, hat_b}));
    assert(!missing_count.matches({}));
    auto aggregate = multi;
    aggregate.state = "count";
    assert(aggregate.matches({a, hat_a, head_a})); // 多个关联类别合计，每个检测框只计一次。
    auto overlap_count = excess;
    overlap_count.relation = "overlap";
    assert(overlap_count.matches({a, hat_a, Detection("y", .9, 90, 150, 20, 20)}));
    assert(!overlap_count.matches({a, hat_a, Detection("y", .9, 91, 150, 20, 20)}));
    for (const auto &invalid_count : {"null", R"({"op":">","value":-1})", R"({"op":">","value":1.5})",
                                   R"({"op":">","value":true})", R"({"op":"bad","value":1})"})
        assert(rejected(rule(count_json(invalid_count))));
    assert(rejected(rule(count_json(R"({"op":">","value":1})", R"(,"one_to_one":true)"))));
    assert(rejected(rule(count_json(R"({"op":">","value":1})", R"(,"one_to_one":false)"))));
    assert(rejected(rule(association_json(R"(,"object_count":{"op":">","value":1})"))));
    assert(rejected(rule(R"({"association":{"subject":{"class":"x"},"object":{"class":"y"},"relation":"near","state":"count"}})")));
    const std::string comparison_json = "{\"compare_counts\":{\"left\":{\"class\":\"x\",\"min_confidence\":0.3},"
                                        "\"right\":{\"class\":\"y\",\"min_confidence\":0.5},\"op\":\">\"}}";
    auto compare = parse_rules(rule(comparison_json), {"x", "y"}, {}, 0.3)[0].condition;
    assert(compare.matches({a, b, hat_a}));
    assert(!compare.matches({a, b, hat_a, hat_b}));
    assert(compare.matches({a, Detection("y", 0.9, 30, 10, 20, 20, false)}));
    assert(!compare.matches({Detection("x", 0.9, 0, 0, 100, 200, false)}));
    auto combined =
        parse_rules(rule("{\"all\":[" + association_json() + "," + comparison_json + "]}"), {"x", "y"}, {}, 0.3)[0]
            .condition;
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
    assert(rejected(
        rule("{\"compare_counts\":{\"left\":{\"class\":\"unknown\"},\"right\":{\"class\":\"y\"},\"op\":\">\"}}")));
    std::vector<Detection> many;
    for (int i = 0; i < 64; ++i)
    {
        many.emplace_back("x", 0.9, i * 100, 0, 90, 200);
        many.emplace_back("y", 0.9, i * 100 + 20, 10, 20, 20);
    }
    auto begin = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i)
        assert(!linked.matches(many));
    std::cout << "64-pair association average us: "
              << std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / 1000
              << '\n';
    const auto any = parse_rules(rule("{\"any\":[" + absent("x") + "," + absent("y") + "]}"), {"x", "y"}, {}, 0.3);
    const auto all = parse_rules(rule("{\"all\":[" + absent("x") + "," + absent("y") + "]}"), {"x", "y"}, {}, 0.3);
    for (int x = 0; x < 2; ++x)
        for (int y = 0; y < 2; ++y)
        {
            std::vector<Detection> ds;
            if (x)
                ds.push_back({"x", 0.8});
            if (y)
                ds.push_back({"y", 0.8});
            assert(any[0].condition.matches(ds) == (!x || !y));
            assert(all[0].condition.matches(ds) == (!x && !y));
        }
    auto negated = parse_rules(rule("{\"not\":" + absent("x") + "}"), {"x"}, {}, 0.3);
    assert(!negated[0].condition.matches({{"x", 0.2}}));
    assert(negated[0].condition.matches({{"x", 0.9}}));
    auto count = parse_rules(rule("{\"class\":\"x\",\"min_confidence\":0.5,\"count\":{\"op\":\">=\",\"value\":2}}"),
                             {"x"}, {}, 0.3);
    assert(!count[0].condition.matches({{"x", 0.8}, {"x", 0.4}}));
    assert(count[0].condition.matches({{"x", 0.8}, {"x", 0.6}}));
    assert(rejected(rule(absent("unknown"))));
    assert(rejected(rule(absent("x")), {"y"}));
    assert(rejected(rule(absent("x")), {}, 0.5));
    assert(rejected(rule("{\"any\":[]}")));
    assert(rejected(rule("{\"all\":[" + absent("x") + "],\"not\":" + absent("x") + "}")));
    assert(rejected(rule("{\"class\":\"x\",\"count\":{\"op\":\"==\",\"value\":-1}}")));
    assert(rejected(rule(absent("x")) + " garbage"));
    std::string deep = absent("x");
    for (int i = 0; i < 10; ++i)
        deep = "{\"not\":" + deep + "}";
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
