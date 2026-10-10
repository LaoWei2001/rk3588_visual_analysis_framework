#include "helmet_guard.h"

#include <cassert>
#include <iostream>

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

    // 原规则保持兼容：没有相交安全帽就违规，包括只看到腿的情况。
    std::vector<AlgoResult> results{person};
    auto out = evaluate(results, zone, old);
    assert(out.alarm && out.triggered && out.unhelmeted_count == 1);
    assert(out.unknown_count == 0 && out.unhelmeted_track_ids == std::vector<int>{11});
    results = {person, detection("helmet", {89, 149, 10, 10})};
    assert(!evaluate(results, zone, old).alarm); // 1 像素相交仍遵循旧规则。
    results = {person, detection("helmet", {90, 0, 20, 20})};
    assert(evaluate(results, zone, old).alarm); // 仅贴边不算相交。
    results = {person, hat, head};
    assert(!evaluate(results, zone, old).alarm); // 旧模式不依赖 bare_head。

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

    std::cout << "Helmet modes, per-person evidence, ROI, thresholds and latch tests passed\n";
}
