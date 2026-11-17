// 一个 cpp 测试正式业务的查询、ROI、跟踪、跨帧状态、事件和绘图。
// 链接 Vision::Analysis；不启动视频主程序、Web、NPU 或事件上传线程。
#include <iostream>
#include <logic_session.h>
#include <opencv2/imgcodecs.hpp>
#include <rendering.h>
#include <tracker.h>

struct IntrusionState
{
    bool occupied = false;
    bool reported = false;
    uint64_t entered_ms = 0;
};

void intrusion(ChannelContext *ctx)
{
    auto *state = ctx->get_state<IntrusionState>();
    TargetQuery query;
    query.labels = {"person"};
    query.min_score = 0.9f;
    query.roi = roi_find(ctx, "entrance");
    query.anchor = TargetAnchor::BottomCenter;
    const auto selected = ctx->query_targets(query);
    if (!selected.valid() || !selected.best)
    {
        *state = IntrusionState{};
        ctx->publish_bool("alarm", false);
        return;
    }
    if (!state->occupied)
    {
        state->entered_ms = ctx->timestamp_ms;
        state->occupied = true;
    }
    const bool alarm = ctx->timestamp_ms >= state->entered_ms && ctx->timestamp_ms - state->entered_ms >= 2000;
    const cv::Scalar color = alarm ? cv::Scalar(0, 0, 255) : cv::Scalar(0, 255, 0);
    draw_rect(ctx, selected.best->box, color);
    draw_polyline(ctx, *ctx->roi_polygon_at(query.roi), color, 2, 1, true);
    ctx->publish_int("count", selected.count);
    ctx->publish_bool("alarm", alarm);
    if (alarm && !state->reported)
    {
        EventRequest request;
        request.event_type = "intrusion";
        request.fields = {event_field("count", selected.count), event_field("track_id", selected.best->track_id)};
        state->reported = report_event(ctx, request).accepted();
    }
}

int main(int argc, char **argv)
{
    cv::Mat source = argc > 1 ? cv::imread(argv[1]) : cv::Mat(720, 1280, CV_8UC3, cv::Scalar(40, 40, 40));
    if (source.empty())
    {
        std::cerr << "Cannot read input image\n";
        return 1;
    }
    vision::LogicSessionOptions options;
    options.channel.id = 0;
    options.logic = intrusion; // 这个函数可以原样注册为正式项目的通道 logic。
    RoiZoneConfig entrance;
    entrance.name = "entrance";
    entrance.polygon = {{.1, .1}, {.9, .1}, {.9, .9}, {.1, .9}};
    options.channel.roi_zones.push_back(entrance);
    vision::LogicSession session(options);
    Tracker tracker(.3f, 10, 1);

    // 可控检测输入，方便快速复现边界情况。实际推理先保留返回对象：
    // auto prediction = inference.infer(source); frame.results = &prediction.results;
    // frame.inference_valid = prediction.succeeded();
    // prediction.results 与这里一样采用 640×640 业务坐标，不是原图像素坐标。
    // prediction 必须存活到下一次 run()；不可借用 infer(source) 临时返回值的地址。
    std::vector<AlgoResult> detections(1);
    detections[0].label = "person";
    detections[0].score = .95f;
    detections[0].box = {200, 200, 100, 200};
    vision::LogicFrame frame;
    frame.source = &source;
    frame.results = &detections;
    for (uint64_t now : {uint64_t(0), uint64_t(1000), uint64_t(2000)})
    {
        tracker.update(detections);
        frame.timestamp_ms = now; // 无需 sleep；直接模拟两秒驻留。
        session.run(frame);
        std::cout << "time_ms=" << now << " captured_events=" << session.events().size() << '\n';
    }
    cv::Mat rendered = source.clone();
    vision::OverlayOptions overlays;
    overlays.inputW = overlays.inputH = 640;
    overlays.show_fps = 0;
    overlays.show_system_overlays = false;
    overlays.draw_cmds = &session.drawing();
    overlays.target_mask = DrawCommand::IMAGE;
    vision::render_overlays(rendered, overlays);
    const std::string output = argc > 2 ? argv[2] : "intrusion-debug.jpg";
    if (!cv::imwrite(output, rendered))
        return 1;
    std::cout << "Rendered: " << output << '\n';
}
