#include "helmet_guard.h"

#include <cassert>
#include <iostream>
#include <limits>

using namespace crane_safety;

static AlgoResult detection(const char *label, cv::Rect box, int id = -1, float score = 0.9f)
{
    AlgoResult result;
    result.label = label;
    result.box = box;
    result.track_id = id;
    result.score = score;
    result.frame_id = 42;
    return result;
}

static HelmetResult evaluate(std::vector<AlgoResult> &detections, const RoiZone &zone, const HelmetConfig &config)
{
    HelmetGuard guard;
    return guard.update(detections, &zone, 1000, config);
}

int main()
{
    RoiZone zone;
    zone.polygon = {{0, 0}, {300, 0}, {300, 300}, {0, 300}};
    HelmetConfig old;
    old.enabled = true;
    old.person_labels = {"person"};
    old.helmet_labels = {"helmet"};
    old.bare_head_labels = {"bare_head"};
    old.confirm_ms = old.clear_ms = 0;
    HelmetConfig positive = old;
    positive.alarm_mode = HelmetConfig::BareHead;
    const AlgoResult person = detection("person", {20, 0, 70, 150}, 11);
    const AlgoResult head = detection("bare_head", {40, 0, 20, 20});
    const AlgoResult hat = detection("helmet", {40, 0, 20, 20});

    // 综合规则：无裸头证据时仍按安全帽匹配判断，裸头证据优先。
    std::vector<AlgoResult> results{person};
    auto out = evaluate(results, zone, old);
    assert(out.alarm && out.triggered && out.unhelmeted_count == 1);
    assert(out.unknown_count == 0 && out.unhelmeted_track_ids == std::vector<int>{11});
    results = {person, detection("helmet", {89, 149, 10, 10})};
    assert(!evaluate(results, zone, old).alarm); // 1 像素相交仍遵循旧规则。
    results = {person, detection("helmet", {90, 0, 20, 20})};
    assert(evaluate(results, zone, old).alarm); // 仅贴边不算相交。
    results = {person, hat, head};
    assert(evaluate(results, zone, old).alarm); // 裸头不被附近安全帽框覆盖。

    // 宽高只限制“没有匹配到安全帽/裸头”的分支，两个边界均包含等号。
    const AlgoResult small = detection("person", {20, 10, 39, 40}, 21);
    const AlgoResult tiny = detection("person", {25, 15, 2, 2}, 22);
    const AlgoResult small_head = detection("bare_head", {30, 10, 2, 2});
    results = {small};
    out = evaluate(results, zone, old);
    assert(!out.alarm && out.person_count == 1 && out.unknown_count == 1 && out.small_person_count == 1);
    assert(out.max_person_width == 39 && out.max_person_height == 40);
    assert(results[0].box_color == cv::Scalar(180, 180, 180));
    results = {detection("person", {20, 10, 40, 40}, 21)};
    assert(evaluate(results, zone, old).alarm);
    results = {detection("person", {20, 10, 41, 40}, 21)};
    assert(evaluate(results, zone, old).alarm);
    results = {detection("person", {20, 10, 40, 39}, 21)};
    assert(!evaluate(results, zone, old).alarm);
    results = {detection("person", {20, 10, 40, 41}, 21)};
    assert(evaluate(results, zone, old).alarm);
    // 面积都超过旧阈值1600，仍必须分别满足宽、高，不能靠另一边补足。
    const AlgoResult narrow = detection("person", {20, 10, 20, 100}, 23);
    const AlgoResult short_person = detection("person", {20, 10, 100, 20}, 24);
    for (const auto &box : {narrow, short_person})
    {
        results = {box};
        out = evaluate(results, zone, old);
        assert(!out.alarm && out.small_person_count == 1 && out.unknown_count == 1);
    }
    auto rectangular_filter = old;
    rectangular_filter.person_min_width = 30;
    rectangular_filter.person_min_height = 80;
    results = {detection("person", {20, 10, 30, 80}, 25)};
    assert(evaluate(results, zone, rectangular_filter).alarm);
    for (const auto &box : {cv::Rect{20, 10, 29, 90}, cv::Rect{20, 10, 40, 79}, cv::Rect{20, 10, 80, 30}})
    {
        results = {detection("person", box, 25)};
        assert(!evaluate(results, zone, rectangular_filter).alarm);
    }
    results = {small, small_head};
    out = evaluate(results, zone, old);
    assert(out.alarm && out.unhelmeted_count == 1 && out.small_person_count == 0);
    assert(out.unhelmeted_track_ids == std::vector<int>{21});
    results = {tiny, detection("bare_head", {25, 15, 1, 1})};
    assert(evaluate(results, zone, old).alarm); // 4 像素人员框、1 像素裸头框同样有效。
    results = {small, detection("helmet", small_head.box), small_head};
    assert(evaluate(results, zone, old).alarm);
    results = {small, detection("helmet", small_head.box)};
    out = evaluate(results, zone, old);
    assert(!out.alarm && out.small_person_count == 0 && out.unknown_count == 0);
    results = {small, detection("bare_head", {180, 10, 2, 2})};
    assert(!evaluate(results, zone, old).alarm); // 别处裸头不能让这个小框违规。
    results = {small, small_head};
    results[1].frame_id = 41;
    assert(!evaluate(results, zone, old).alarm);
    results = {small, detection("bare_head", small_head.box, -1, 0.2f)};
    assert(!evaluate(results, zone, old).alarm);
    results = {small, person, small_head};
    out = evaluate(results, zone, old);
    assert(out.person_count == 2 && out.unhelmeted_count == 2); // 每个人分别采用自己的证据。
    auto no_size_filter = old;
    no_size_filter.person_min_width = no_size_filter.person_min_height = 0;
    results = {small};
    assert(evaluate(results, zone, no_size_filter).alarm);
    results = {tiny};
    assert(evaluate(results, zone, no_size_filter).alarm);
    auto height_only = old;
    height_only.person_min_width = 0;
    results = {narrow};
    assert(evaluate(results, zone, height_only).alarm);
    results = {short_person};
    assert(!evaluate(results, zone, height_only).alarm);
    auto width_only = old;
    width_only.person_min_height = 0;
    assert(evaluate(results, zone, width_only).alarm);
    results = {narrow};
    assert(!evaluate(results, zone, width_only).alarm);
    results = {person, hat};
    results[1].frame_id = 41;
    assert(evaluate(results, zone, old).alarm); // 旧帧安全帽也不能抵消本帧违规。
    results = {detection("person", {20, 10, 0, 40}), small_head};
    assert(evaluate(results, zone, old).person_count == 0);
    results = {small, detection("bare_head", small_head.box, -1, std::numeric_limits<float>::quiet_NaN())};
    assert(!evaluate(results, zone, old).alarm);

    // 新规则不把“缺少头部证据”当成违规，也不把它标成戴帽。
    results = {person};
    out = evaluate(results, zone, positive);
    assert(!out.alarm && out.person_count == 1 && out.unhelmeted_count == 0 && out.unknown_count == 1);
    assert(results[0].box_color == cv::Scalar(180, 180, 180));
    results = {person, hat};
    out = evaluate(results, zone, positive);
    assert(!out.alarm && out.unknown_count == 0 && results[0].box_color == cv::Scalar(0, 200, 0));
    results = {person, head};
    out = evaluate(results, zone, positive);
    assert(out.alarm && out.unhelmeted_count == 1 && out.unknown_count == 0);
    assert(results[0].box_color == cv::Scalar(0, 0, 255));
    auto huge_size_filter = positive;
    huge_size_filter.person_min_width = huge_size_filter.person_min_height = 640;
    results = {tiny, detection("bare_head", {25, 15, 1, 1})};
    assert(evaluate(results, zone, huge_size_filter).alarm); // 裸头模式不受宽高影响。
    huge_size_filter.alarm_mode = HelmetConfig::MissingHelmet;
    assert(evaluate(results, zone, huge_size_filter).alarm); // 综合模式下裸头同样绕过两个阈值。
    results = {person, hat, head};
    assert(evaluate(results, zone, positive).alarm); // 正面裸头证据不被附近帽框覆盖。
    results = {head};
    assert(!evaluate(results, zone, positive).alarm);
    results = {person, detection("bare_head", {80, 0, 40, 20})};
    assert(!evaluate(results, zone, positive).alarm); // 头框中心不属于此人。
    results = {person, detection("bare_head", {40, 0, 20, 20}, -1, 0.2f)};
    assert(!evaluate(results, zone, positive).alarm);
    results = {detection("person", person.box, 11, 0.2f), head};
    assert(!evaluate(results, zone, positive).alarm);
    results = {detection("person", {320, 0, 70, 150}, 12), detection("bare_head", {340, 0, 20, 20})};
    assert(evaluate(results, zone, positive).person_count == 0);
    results = {person, head};
    results[1].frame_id = 41;
    assert(!evaluate(results, zone, positive).alarm); // 不使用旧帧头部结果。
    results = {person, detection("bare_head", {40, 0, 0, 20})};
    assert(!evaluate(results, zone, positive).alarm);

    // 多人逐人匹配，一个裸头不会把旁边的人一起标成违规。
    results = {person, detection("person", {180, 0, 70, 150}, 12), head};
    out = evaluate(results, zone, positive);
    assert(out.person_count == 2 && out.unhelmeted_count == 1 && out.unknown_count == 1);
    assert(out.unhelmeted_track_ids == std::vector<int>{11});
    results.push_back(detection("helmet", {200, 0, 20, 20}));
    out = evaluate(results, zone, positive);
    assert(out.unhelmeted_count == 1 && out.unknown_count == 0);
    results = {detection("person", {10, 0, 100, 180}, 13), person, head, head};
    out = evaluate(results, zone, positive);
    assert(out.unhelmeted_count == 1 && out.unhelmeted_track_ids == std::vector<int>{11});

    // 自定义类别名，不把裸头标签硬编码在判定逻辑中。
    auto custom = positive;
    custom.bare_head_labels = {"bore_head", "uncovered_head"};
    results = {person, detection("bore_head", head.box)};
    assert(evaluate(results, zone, custom).alarm);

    // 保留持续确认和延迟解除：头部消失后解除计时，断续证据不累计。
    auto timed = positive;
    timed.confirm_ms = 500;
    timed.clear_ms = 1000;
    HelmetGuard guard;
    results = {person, head};
    assert(!guard.update(results, &zone, 1000, timed).alarm);
    assert(!guard.update(results, &zone, 1499, timed).alarm);
    assert(guard.update(results, &zone, 1500, timed).triggered);
    results = {person};
    assert(guard.update(results, &zone, 1600, timed).alarm);
    assert(guard.update(results, &zone, 2599, timed).alarm);
    out = guard.update(results, &zone, 2600, timed);
    assert(!out.alarm && out.cleared && out.unknown_count == 1);
    results = {person, head};
    assert(!guard.update(results, &zone, 3000, timed).alarm);
    results = {person};
    assert(!guard.update(results, &zone, 3400, timed).alarm);
    results = {person, head};
    assert(!guard.update(results, &zone, 3500, timed).alarm);
    assert(!guard.update(results, &zone, 3999, timed).alarm);
    assert(guard.update(results, &zone, 4000, timed).alarm);
    guard.reset();
    assert(!guard.update(results, &zone, 4100, timed).alarm);

    // 切换模式后不继承旧告警或确认计时。
    results = {person};
    assert(guard.update(results, &zone, 5000, old).alarm);
    assert(!guard.update(results, &zone, 5100, positive).alarm);
    auto timed_old = old;
    timed_old.confirm_ms = 500;
    assert(!guard.update(results, &zone, 6000, timed_old).alarm);
    results = {person, head};
    assert(!guard.update(results, &zone, 6400, timed).alarm);
    assert(!guard.update(results, &zone, 6899, timed).alarm);
    assert(guard.update(results, &zone, 6900, timed).alarm);
    assert(!guard.update(results, nullptr, 7000, positive).alarm);
    positive.enabled = false;
    assert(!evaluate(results, zone, positive).alarm);

    // 综合模式中小框不积累违规时间，变成裸头证据后才开始确认。
    auto timed_size = old;
    timed_size.confirm_ms = 500;
    timed_size.clear_ms = 1000;
    guard.reset();
    results = {small};
    assert(!guard.update(results, &zone, 10000, timed_size).alarm);
    assert(!guard.update(results, &zone, 11000, timed_size).alarm);
    results = {small, small_head};
    assert(!guard.update(results, &zone, 11100, timed_size).alarm);
    assert(!guard.update(results, &zone, 11599, timed_size).alarm);
    assert(guard.update(results, &zone, 11600, timed_size).triggered);
    results = {small};
    assert(guard.update(results, &zone, 12000, timed_size).alarm);
    assert(guard.update(results, &zone, 12999, timed_size).alarm);
    assert(guard.update(results, &zone, 13000, timed_size).cleared);
    results = {person};
    assert(!guard.update(results, &zone, 14000, timed_size).alarm);
    auto changed_size = timed_size;
    changed_size.person_min_width = 80;
    assert(!guard.update(results, &zone, 14500, changed_size).alarm);
    assert(!guard.update(results, &zone, 15000, changed_size).alarm);

    // 即使人员仍满足新阈值，修改任何一边都要清除旧告警和确认时间。
    guard.reset();
    assert(!guard.update(results, &zone, 16000, timed_size).alarm);
    assert(guard.update(results, &zone, 16500, timed_size).alarm);
    changed_size = timed_size;
    changed_size.person_min_width = 41;
    assert(!guard.update(results, &zone, 16600, changed_size).alarm);
    assert(!guard.update(results, &zone, 17099, changed_size).alarm);
    assert(guard.update(results, &zone, 17100, changed_size).triggered);
    changed_size.person_min_height = 41;
    assert(!guard.update(results, &zone, 17200, changed_size).alarm);
    assert(!guard.update(results, &zone, 17699, changed_size).alarm);
    assert(guard.update(results, &zone, 17700, changed_size).triggered);

    std::cout << "Helmet modes, per-person evidence, ROI, thresholds and latch tests passed\n";
}
