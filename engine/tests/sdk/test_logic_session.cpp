#include <bytetrack.h>
#include <cassert>
#include <cstring>
#include <drawing.h>
#include <iostream>
#include <logic_session.h>
#include <rendering.h>
#include <stdexcept>
#include <tracker.h>

namespace
{
struct State
{
    int visits = 0;
};

void inspect(ChannelContext *ctx)
{
    ++ctx->get_state<State>()->visits;
    TargetQuery query;
    query.labels = {"person"};
    query.min_score = 0.9f;
    query.roi = ROI_ALL;
    const auto selected = ctx->query_targets(query);
    const auto direct =
        vision::query_targets(ctx->results, ctx->rois, query, ctx->inference_valid && ctx->infer_enabled);
    assert(selected.status == direct.status && selected.count == direct.count && selected.best == direct.best);
    ctx->publish_int("visits", ctx->get_state<State>()->visits);
    ctx->publish_int("targets", selected.count);
    if (!selected.best)
        return;
    assert(selected.best == &ctx->results->front()); // 直接借用输入，无复制。
    draw_rect(ctx, selected.best->box, {0, 0, 255});
    EventRequest request;
    request.event_type = "intrusion";
    assert(report_event(ctx, request).accepted());
    if (ctx->source_frame())
    {
        const cv::Mat *first = ctx->business_frame();
        assert(first && first == ctx->business_frame() && first->size() == ctx->business_size());
        assert(ctx->replace_display_frame(*first));
    }
}

LogicActionResult clear(ChannelContext *ctx, const LogicAction *action)
{
    LogicActionResult result;
    if (action && action->name == "clear")
    {
        ctx->get_state<State>()->visits = 0;
        result.handled = true;
    }
    return result;
}

int64_t integer(const vision::LogicSession &session, const char *key)
{
    int64_t value = -1;
    assert(session.outputs().try_get_int(key, &value));
    return value;
}

void registry_owns_names()
{
    char logic_name[] = "mutable_logic";
    char action_name[] = "mutable_logic";
    register_logic(logic_name, inspect);
    register_logic_action(action_name, clear);
    std::memset(logic_name, 'x', sizeof(logic_name) - 1);
    std::memset(action_name, 'y', sizeof(action_name) - 1);
    register_logic(std::string("temporary_logic").c_str(), inspect);
    register_logic_action(std::string("temporary_logic").c_str(), clear);
    // 命名回调与正式模块使用同一 Schema 入口；无参数模块声明空 properties。
    vision::set_logic_catalog(R"({"channel_logics":[
        {"name":"mutable_logic","parameters":{"properties":{},"additionalProperties":false}},
        {"name":"temporary_logic","parameters":{"properties":{},"additionalProperties":false}}
    ],"global_logics":[]})");
    for (const char *name : {"mutable_logic", "temporary_logic"})
    {
        vision::LogicSessionOptions options;
        options.channel.logic = name; // 仅按注册名称查找，不直接传 callback。
        vision::LogicSession session(options);
        vision::LogicFrame frame;
        session.run(frame);
        assert(integer(session, "visits") == 1);
        LogicAction action;
        action.name = "clear";
        assert(session.dispatch(action).handled);
        session.run(frame);
        assert(integer(session, "visits") == 1);
    }
}

void analysis_time_follows_video()
{
    vision::LogicSessionOptions options;
    options.logic = [](ChannelContext *ctx) { ctx->publish_int("video_time_ms", ctx->video_time_ms); };
    vision::LogicSession session(options);
    vision::LogicFrame frame;
    std::vector<AlgoResult> results(1);
    frame.results = &results;
    session.run(frame);
    assert(integer(session, "video_time_ms") == -1);
    frame.video_time_ms = 0; // 文件开头的 0 是有效时间。
    frame.timestamp_ms = 10000;
    session.run(frame);
    assert(integer(session, "video_time_ms") == 0);
    assert(session.context().dt_ms == 0);
    // 现实时间跨过一分钟，业务仍沿用 timestamp_ms/dt_ms，只计视频经过的一帧。
    frame.video_time_ms = 33;
    frame.timestamp_ms = 70033;
    session.run(frame);
    assert(integer(session, "video_time_ms") == 33);
    assert(session.context().timestamp_ms == 34 && session.context().dt_ms == 33);
    assert(session.context().real_time_ms == 70033);
    assert(results.front().timestamp_ms == session.context().timestamp_ms);
    frame.video_time_ms = 0; // 跳回文件开头时原样传递，不伪造递增时间。
    session.run(frame);
    assert(integer(session, "video_time_ms") == 0);
    assert(session.context().dt_ms == 0);
    frame.video_time_ms = -1; // 后一帧未提供时不能残留前一帧的位置。
    session.run(frame);
    assert(integer(session, "video_time_ms") == -1);
    assert(session.context().dt_ms == 0);
}

void frame_inference_state_controls_queries()
{
    vision::LogicSessionOptions options;
    options.logic = inspect;
    vision::LogicSession session(options);
    std::vector<AlgoResult> results(1);
    results[0].label = "person";
    results[0].score = .95f;
    results[0].box = {20, 30, 50, 100};
    vision::LogicFrame frame;
    frame.results = &results;
    frame.inference_enabled = false;
    session.run(frame);
    assert(!session.context().infer_enabled && integer(session, "targets") == 0);
    assert(session.drawing().empty());
    frame.inference_enabled = true;
    session.run(frame);
    assert(session.context().infer_enabled && integer(session, "targets") == 1);
    assert(session.drawing().size() == 1);
    frame.inference_enabled = false;
    session.run(frame);
    assert(integer(session, "targets") == 0 && session.drawing().empty());
}
} // namespace

int main()
{
    // A default context is deterministic and cannot dereference an unbound canvas.
    ChannelContext unbound;
    assert(unbound.frame_id == 0 && unbound.timestamp_ms == 0 && unbound.dt_ms == 0);
    assert(unbound.video_time_ms == -1);
    assert(unbound.config == nullptr && unbound.infer_fps == 0 && unbound.disp_fps == 0);
    assert(unbound.business_frame() == nullptr && unbound.source_frame() == nullptr);
    bool canvas_rejected = false;
    try
    {
        (void)unbound.display_canvas();
    }
    catch (const std::logic_error &)
    {
        canvas_rejected = true;
    }
    assert(canvas_rejected);
    cv::Mat unbound_mask(2, 2, CV_8UC1, cv::Scalar(1));
    assert(!blend_display_mask(&unbound, unbound_mask, cv::Scalar(0, 0, 255), 0.5));
    assert(!unbound.replace_display_frame(cv::Mat(2, 2, CV_8UC3)));

    registry_owns_names();
    analysis_time_follows_video();
    frame_inference_state_controls_queries();
    vision::LogicSessionOptions options;
    options.logic = inspect;
    options.action = clear;
    RoiZoneConfig configured;
    configured.name = "gate";
    configured.polygon = {{0, 0}, {0.5, 0}, {0.5, 0.5}, {0, 0.5}, {0, 0}};
    options.channel.roi_zones.push_back(configured);
    vision::LogicSession first(options), second(options);
    std::vector<AlgoResult> results(1);
    results[0].label = "person";
    results[0].score = .95f;
    results[0].box = {20, 30, 50, 100};
    cv::Mat source(720, 1280, CV_8UC3, cv::Scalar(20, 30, 40));
    const cv::Mat original = source.clone();
    vision::LogicFrame input;
    input.source = &source;
    input.results = &results;
    first.run(input);
    assert(first.context().src_width == 1280 && first.context().src_height == 720);
    assert(first.context().roi_at(0)->polygon.size() == 4);
    assert(first.context().roi_at(0)->polygon[2] == cv::Point(320, 320));
    assert(integer(first, "visits") == 1 && integer(first, "targets") == 1);
    TargetQuery query;
    query.labels = {"person"};
    query.roi = ROI_ALL;
    assert(first.context().query_targets(query).count == 1);
    assert(first.events().size() == 1 && first.events()[0].drawing.size() == 1);
    assert(first.display_frame() && first.display_frame()->size() == cv::Size(640, 640));
    assert(cv::norm(source, original, cv::NORM_INF) == 0);

    input.timestamp_ms = 500;
    input.inference_valid = false;
    first.run(input);
    assert(integer(first, "visits") == 2 && integer(first, "targets") == 0);
    // 容器仍保留上帧目标，但所有目标查询都必须尊重本帧的失效标记。
    assert(results.size() == 1);
    const auto unavailable = first.context().query_targets(query);
    assert(unavailable.status == TargetQueryStatus::INFERENCE_UNAVAILABLE && unavailable.count == 0);
    assert(first.context().dt_ms == 500 && first.events().empty() && first.drawing().empty());
    assert(!first.display_frame());
    second.run(input);
    assert(integer(second, "visits") == 1);
    LogicAction action;
    action.name = "clear";
    assert(first.dispatch(action).handled);
    first.run(input);
    assert(integer(first, "visits") == 1);
    first.reset_state();
    first.run(input);
    assert(integer(first, "visits") == 1);

    // 错误输入不会运行回调；错误参数不覆盖此前已生效的配置。
    cv::Mat invalid(2, 2, CV_8UC1);
    input.source = &invalid;
    bool rejected = false;
    try
    {
        first.run(input);
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    assert(rejected && integer(first, "visits") == 1);
    rejected = false;
    try
    {
        first.set_parameters("{\"unknown\":1}");
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    assert(rejected && first.context().config->logic_parameters_json == "{}");

    // 直接公开像素绘制与上下文绘制复用同一 mask 内核。
    cv::Mat bgr(10, 10, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat mask(10, 10, CV_8UC1, cv::Scalar(255));
    assert(vision::blend_mask(bgr, mask, {0, 0, 200}, .5));
    assert(bgr.at<cv::Vec3b>(5, 5) == cv::Vec3b(0, 0, 100));
    Tracker sort(.3f, 10, 1);
    ByteTracker byte(.3f, .2f, .1f, 10, 1, .9f);
    sort.update(results);
    assert(results[0].track_id > 0);
    results[0].track_id = -1;
    byte.update(results);
    assert(results[0].track_id > 0);
    std::cout << "Public analysis SDK: query, ROI, replay, action, event, mask, tracking passed\n";
}
