// 外部业务消费者：只包含公开 SDK，执行引擎的真实查询实现。
#include <cassert>
#include <channel.h>
#include <iostream>
#include <limits>

namespace
{
AlgoResult detection(const char *label, float score, cv::Rect box = {10, 10, 20, 20})
{
    AlgoResult result;
    result.label = label;
    result.score = score;
    result.box = box;
    return result;
}

RoiZone square(const char *name, int x = 0)
{
    RoiZone zone;
    zone.name = name;
    zone.polygon = {{x, 0}, {x + 100, 0}, {x + 100, 100}, {x, 100}};
    return zone;
}

struct Fixture
{
    ChannelContext ctx{};
    std::vector<AlgoResult> results;
    std::vector<RoiZone> rois{square("entrance")};

    Fixture()
    {
        ctx.results = &results;
        ctx.rois = &rois;
        ctx.infer_enabled = 1;
        ctx.inference_valid = true;
    }
};

void expect_failure(const TargetQueryResult &result, TargetQueryStatus status)
{
    assert(result.status == status);
    assert(!result.valid() && result.count == 0 && result.best == nullptr);
}

void expect_unavailable(const ChannelContext &ctx)
{
    TargetQuery query;
    query.labels = {"person"};
    expect_failure(ctx.query_targets(query), TargetQueryStatus::INFERENCE_UNAVAILABLE);
    query.roi = ROI_ALL;
    expect_failure(ctx.query_targets(query), TargetQueryStatus::INFERENCE_UNAVAILABLE);
}

void filters_and_best()
{
    Fixture f;
    f.results = {detection("person", .8f), detection("person", .95f, {200, 10, 20, 20}), detection("car", .9f),
                 detection("dog", .1f), detection("person", .7f)};
    auto result = f.ctx.query_targets();
    assert(result.valid() && result.count == 5 && result.best == &f.results[1]);
    TargetQuery q;
    q.labels = {"person", "car", "person"}; // 重复标签不重复计数。
    q.min_score = .8f;
    result = f.ctx.query_targets(q);
    assert(result.valid() && result.count == 3 && result.best == &f.results[1]);
    q.roi = roi_find(&f.ctx, "entrance");
    result = f.ctx.query_targets(q);
    assert(result.valid() && result.count == 2 && result.best == &f.results[2]);
    q.labels = {"missing"};
    result = f.ctx.query_targets(q);
    assert(result.valid() && result.count == 0 && result.best == nullptr);
    q.labels.clear();
    q.min_score = 1;
    f.results = {detection("person", 1), detection("car", .999f), detection("dog", 1)};
    result = f.ctx.query_targets(q);
    assert(result.count == 2 && result.best == &f.results[0]); // 同分取第一个。
    f.results.clear();
    result = f.ctx.query_targets(q);
    assert(result.valid() && result.count == 0 && !result.best);
}

void regions_and_union()
{
    Fixture f;
    f.rois.push_back(square("overlap", 50));
    f.results = {detection("person", .8f, {65, 10, 20, 20}), detection("person", .9f, {200, 10, 20, 20})};
    TargetQuery q;
    q.roi = ROI_ALL;
    auto result = f.ctx.query_targets(q);
    assert(result.valid() && result.count == 1); // 两个区域重叠也只统计一次。
    q.roi = ROI_FRAME;
    assert(f.ctx.query_targets(q).count == 2);
    q.roi = roi_find(&f.ctx, "unknown");
    expect_failure(f.ctx.query_targets(q), TargetQueryStatus::ROI_NOT_FOUND);
    q.roi = 99;
    expect_failure(f.ctx.query_targets(q), TargetQueryStatus::ROI_NOT_FOUND);
    q.roi = 0;
    f.rois[0].polygon.resize(2);
    expect_failure(f.ctx.query_targets(q), TargetQueryStatus::INVALID_ROI);
    q.roi = ROI_ALL;
    expect_failure(f.ctx.query_targets(q), TargetQueryStatus::INVALID_ROI);
    q.roi = 1;
    assert(f.ctx.query_targets(q).count == 1); // 单独查询仍然有效的区域。
    q.roi = ROI_FRAME;
    assert(f.ctx.query_targets(q).count == 2); // 整帧查询不依赖区域配置。
    f.rois.clear();
    q.roi = ROI_ALL;
    assert(f.ctx.query_targets(q).count == 2); // 保留 ROI_ALL 无区域时的整帧语义。
    f.ctx.rois = nullptr;
    assert(f.ctx.query_targets(q).count == 2);
    q.roi = 0;
    expect_failure(f.ctx.query_targets(q), TargetQueryStatus::ROI_NOT_FOUND);
}

void anchor_and_boundary()
{
    Fixture f;
    f.results = {detection("person", .8f, {20, 70, 20, 60}), detection("person", .9f, {20, -40, 20, 60})};
    TargetQuery q;
    q.roi = 0;
    auto result = f.ctx.query_targets(q);
    assert(result.count == 1 && result.best == &f.results[0]); // 中心 y=100，边界包含。
    q.anchor = TargetAnchor::BottomCenter;
    result = f.ctx.query_targets(q);
    assert(result.count == 1 && result.best == &f.results[1]);
    q.anchor = TargetAnchor::Center;
    f.results = {detection("person", .9f, {-10, 0, 20, 20}), detection("person", .8f, {20, 90, 21, 21})};
    result = f.ctx.query_targets(q);
    assert(result.count == 2); // 包含左边界，奇数宽高使用整数中心。
    f.results = {detection("person", .9f, {std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), 50, 50})};
    assert(f.ctx.query_targets(q).count == 0); // 坐标加法不能整数溢出。
    assert(roi_contains(&f.ctx, f.results[0].box, 0) == 0);
}

void availability_and_precedence()
{
    ChannelContext unbound;
    expect_unavailable(unbound);
    Fixture f;
    f.results = {detection("person", .8f)};
    f.ctx.inference_valid = false;
    expect_unavailable(f.ctx);
    f.ctx.inference_valid = true;
    f.ctx.infer_enabled = 0;
    expect_unavailable(f.ctx);
    f.ctx.infer_enabled = 1;
    f.ctx.results = nullptr;
    expect_unavailable(f.ctx);
    TargetQuery q;
    q.roi = ROI_NONE;
    expect_failure(f.ctx.query_targets(q), TargetQueryStatus::ROI_NOT_FOUND);
    q.min_score = -1;
    expect_failure(f.ctx.query_targets(q), TargetQueryStatus::INVALID_QUERY);
}

void invalid_values()
{
    Fixture f;
    TargetQuery q;
    for (float score : {-0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()})
    {
        q.min_score = score;
        expect_failure(f.ctx.query_targets(q), TargetQueryStatus::INVALID_QUERY);
    }
    q.min_score = 0;
    q.anchor = static_cast<TargetAnchor>(99);
    expect_failure(f.ctx.query_targets(q), TargetQueryStatus::INVALID_QUERY);
    q.anchor = TargetAnchor::Center;
    q.roi = -99;
    expect_failure(f.ctx.query_targets(q), TargetQueryStatus::INVALID_QUERY);
    q.roi = ROI_FRAME;
    f.results = {detection("person", std::numeric_limits<float>::quiet_NaN()),
                 detection("person", std::numeric_limits<float>::infinity()),
                 detection("person", -std::numeric_limits<float>::infinity()),
                 detection("person", 1.1f),
                 detection("person", -.1f),
                 detection("person", .9f, {10, 10, 0, 20}),
                 detection("person", .9f, {10, 10, 20, -1}),
                 detection("person", 0)};
    const auto result = f.ctx.query_targets(q);
    assert(result.valid() && result.count == 1 && result.best == &f.results.back());
    auto depth = detection("person", 1);
    depth.depth_map = cv::Mat::ones(1, 1, CV_32FC1);
    f.results.push_back(depth);
    const int regions[] = {ROI_FRAME, ROI_ALL, 0};
    for (int roi : regions)
    {
        q.roi = roi;
        assert(f.ctx.query_targets(q).count == 1);
    }
    f.results.erase(f.results.end() - 2); // 删除唯一合法检测，余下异常数据和深度载荷。
    assert(f.ctx.query_targets(q).valid() && f.ctx.query_targets(q).count == 0);
}

void isolation_and_geometry()
{
    Fixture a, b;
    a.results = {detection("person", .8f), detection("person", .9f, {200, 10, 20, 20})};
    b.results = {detection("car", .7f)};
    assert(a.ctx.query_targets().best == &a.results[1]);
    assert(b.ctx.query_targets().best == &b.results[0]);
    assert(roi_contains(&a.ctx, a.results[0].box, ROI_ALL) == 1);
    assert(roi_contains(&a.ctx, a.results[1].box, ROI_ALL) == 0);
    assert(roi_contains(&a.ctx, a.results[1].box, ROI_FRAME) == 1);
    assert(roi_contains(nullptr, a.results[0].box, ROI_FRAME) == 0);
    assert(roi_contains(&a.ctx, a.results[0].box, ROI_NONE) == 0);
    assert(roi_contains(&a.ctx, a.results[0].box, -99) == 0);
    assert(roi_contains(&a.ctx, a.results[0].box, 99) == 0);
    a.ctx.inference_valid = false;
    expect_unavailable(a.ctx);
    assert(roi_contains(&a.ctx, a.results[0].box, 0) == 1); // 几何查询独立于推理状态。
    assert(b.ctx.query_targets().valid() && b.ctx.query_targets().count == 1);
    a.rois.clear();
    expect_unavailable(a.ctx);
    assert(roi_contains(&a.ctx, a.results[1].box, ROI_ALL) == 1);
    a.ctx.inference_valid = true;
    TargetQuery query;
    query.labels = {"missing"};
    assert(a.ctx.query_targets(query).count == 0);
    query.labels = {""}; // 空字符串按字面匹配，空标签数组才表示所有类别。
    assert(a.ctx.query_targets(query).count == 0);
    a.results.push_back(detection("", .5f));
    assert(a.ctx.query_targets(query).count == 1);
    a.results.clear();
    assert(a.ctx.query_targets().valid() && a.ctx.query_targets().count == 0);
    assert(std::string(target_query_status_name(TargetQueryStatus::OK)) == "ok");
    assert(std::string(target_query_status_name(TargetQueryStatus::INVALID_ROI)) == "invalid_roi");
    assert(std::string(target_query_status_name(static_cast<TargetQueryStatus>(99))) == "unknown");
}
} // namespace

int main()
{
    Fixture scoped;
    scoped.results = {detection("person", .8f), detection("person", .9f)};
    scoped.results[0].model_id = "a";
    scoped.results[0].model_type = "yolov8_det";
    scoped.results[1].model_id = "b";
    scoped.results[1].model_type = "yolov8_obb";
    TargetQuery query;
    query.model_id = "a";
    assert(scoped.ctx.query_targets(query).best == &scoped.results[0]);
    query.model_type = "yolov8_obb";
    assert(scoped.ctx.query_targets(query).count == 0);
    query.model_id = "b";
    assert(scoped.ctx.query_targets(query).best == &scoped.results[1]);
    scoped.results[1].boxMask = cv::Mat::ones(640, 640, CV_8UC1);
    assert(scoped.ctx.segmentation_result("b") == &scoped.results[1]);
    assert(!scoped.ctx.segmentation_result("a"));
    scoped.ctx.inference_valid = false;
    assert(!scoped.ctx.segmentation_result("b"));
    filters_and_best();
    regions_and_union();
    anchor_and_boundary();
    availability_and_precedence();
    invalid_values();
    isolation_and_geometry();
    std::cout << "Target query: filters, scores, ROI, anchors, validity, borrowing and isolation passed\n";
}
