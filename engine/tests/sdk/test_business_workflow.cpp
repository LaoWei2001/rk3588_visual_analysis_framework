// 测试夹具可以绑定内部上下文；被测业务单独编译，只获得 Vision::SDK include 路径。
#include "display/display.h"
#include "logic/core/context_access.h"
#include "logic/core/logic_parameters.h"
#include "logic/core/logic_registry.h"
#include <cassert>
#include <cmath>
#include <events.h>
#include <fstream>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

namespace
{
constexpr const char *logic_name = "logic_roi_dwell_demo";
struct SubmittedEvent
{
    int channel;
    uint64_t time;
    EventRequest request;
    std::vector<DrawCommand> drawing;
};
std::vector<SubmittedEvent> submissions;
EventReportStatus next_status = EventReportStatus::CREATED;

AlgoResult target(int x = 180, const char *label = "person")
{
    AlgoResult result;
    result.label = label;
    result.box = {x, 200, 40, 80};
    result.score = .95f;
    return result;
}

struct Fixture
{
    ChannelContext ctx{};
    LogicParameterSet parameters;
    LogicOutputSet outputs;
    std::shared_ptr<void> state;
    std::vector<DrawCommand> drawing;
    std::vector<AlgoResult> results;
    std::vector<RoiZone> rois;
    std::string json = "{}";
    ChannelLogicFunc run = channel_logic_get(logic_name);

    explicit Fixture(int channel = 0)
    {
        assert(run);
        RoiZone zone;
        zone.name = "alarm_zone";
        zone.polygon = {{100, 100}, {500, 100}, {500, 500}, {100, 500}};
        rois.push_back(zone);
        ctx.chnId = channel;
        ctx.state = &state;
        ctx.results = &results;
        ctx.rois = &rois;
        ctx.infer_enabled = 1;
        VisionContextAccess::bind_outputs(ctx, &outputs);
        VisionContextAccess::bind_drawing(ctx, &drawing);
        configure("{}");
    }

    void configure(const std::string &value)
    {
        LogicParameterSet updated;
        std::vector<LogicParameterError> errors;
        assert(logic_parameters_resolve(logic_name, value, nullptr, &updated, &errors));
        // 与运行时相同的 schema 影响计算；这里不启动 Web/配置监视线程。
        if (logic_parameters_reload_impact(logic_name, json, value) == LogicReloadImpact::RESET_STATE)
            state.reset();
        parameters = std::move(updated);
        json = value;
        VisionContextAccess::bind_parameters(ctx, &parameters);
    }

    void frame(uint64_t now, std::vector<AlgoResult> detections = {target()}, bool valid = true)
    {
        results = std::move(detections);
        outputs = LogicOutputSet{};
        drawing.clear();
        ++ctx.frame_id;
        ctx.timestamp_ms = now;
        ctx.inference_valid = valid;
        run(&ctx);
        assert(outputs.size() == 5); // 空帧也应发布完整的当前帧状态，不能留下旧值。
    }

    int64_t integer(const char *key) const
    {
        int64_t value = -1;
        assert(outputs.try_get_int(key, &value));
        return value;
    }
    bool boolean(const char *key) const
    {
        bool value = false;
        assert(outputs.try_get_bool(key, &value));
        return value;
    }
    std::string status() const
    {
        std::string value;
        assert(outputs.try_get_string("status", &value));
        return value;
    }
    void occupy(uint64_t start, uint64_t end)
    {
        for (uint64_t now = start; now <= end; now += 500)
            frame(now);
    }
};

void boundary_and_payload()
{
    Fixture f;
    f.frame(0, {target(), target(300), target(550), target(200, "car")});
    assert(f.integer("target_count") == 2);
    f.occupy(500, 1500);
    f.frame(1999);
    assert(!f.boolean("alarm_active") && submissions.empty());
    f.frame(2000);
    assert(f.boolean("alarm_active") && f.boolean("event_accepted"));
    assert(submissions.size() == 1);
    const auto &event = submissions.front();
    assert(event.channel == 0 && event.time == 2000);
    assert(event.request.event_type == "roi_dwell");
    assert(event.request.fields.values().at("dwell_ms").number() == 2000);
    assert(event.request.fields.values().at("target_count").number() == 1);
    assert(event.request.fields.values().at("target_label").text() == "person");
    assert(event.request.fields.values().at("roi_name").text() == "alarm_zone");
    assert(event.drawing.size() == 3); // 提交时本帧绘图已存在。
    assert(event.drawing[0].closed && event.drawing[0].color == cv::Scalar(0, 0, 255));
    assert(event.drawing.back().text.find("alarm count=1 dwell=2000ms") != std::string::npos);
    f.occupy(2500, 5000);
    assert(submissions.size() == 1); // 不逐帧重复上报。
    f.frame(5500, {});
    assert(!f.boolean("alarm_active") && !f.boolean("event_accepted"));
    f.occupy(6000, 8000);
    assert(submissions.size() == 2); // 离开后重新进入可以再次报警。
}

void missing_and_invalid_rois()
{
    for (int kind = 0; kind < 3; ++kind)
    {
        Fixture f;
        if (kind == 0)
            f.rois.clear();
        if (kind == 1)
            f.rois[0].name = "other";
        if (kind == 2)
            f.rois[0].polygon.resize(2);
        f.occupy(0, 3000);
        assert(f.status() == "roi_missing" && f.integer("target_count") == 0);
        assert(!f.boolean("alarm_active") && submissions.empty());
    }
    Fixture f;
    f.frame(0, {target(80)}); // 框中心在 x=100 边界，SDK 包含边界。
    assert(f.integer("target_count") == 1);
    f.frame(500, {target(50), target(200, "car")});
    assert(f.status() == "waiting" && f.integer("target_count") == 0);
}

void interruptions()
{
    for (int kind = 0; kind < 4; ++kind)
    {
        Fixture f;
        f.occupy(0, 1000);
        if (kind == 0)
            f.frame(1500, {}); // 单帧漏检也是中断。
        if (kind == 1)
            f.frame(1500, {target()}, false);
        if (kind == 2)
        {
            f.ctx.infer_enabled = 0;
            f.frame(1500);
            f.ctx.infer_enabled = 1;
        }
        if (kind == 3)
        {
            f.ctx.results = nullptr;
            f.frame(1500);
            f.ctx.results = &f.results;
        }
        assert(!f.boolean("alarm_active") && f.integer("dwell_ms") == 0);
        if (kind != 0)
            assert(f.status() == "inference_unavailable");
        f.frame(2000);
        assert(f.integer("dwell_ms") == 0 && submissions.empty());
    }
    Fixture f;
    f.occupy(1000, 2000);
    f.frame(900); // 时间倒退，不得发生 unsigned 下溢报警。
    assert(f.integer("dwell_ms") == 0);
    f.frame(10000); // 中间没有回调，也不能把断流计入持续出现。
    assert(f.integer("dwell_ms") == 0 && submissions.empty());
}

void retry_and_acceptance()
{
    for (auto accepted :
         {EventReportStatus::CREATED, EventReportStatus::MERGED, EventReportStatus::CREATED_MEDIA_FAILED})
    {
        submissions.clear();
        Fixture f;
        next_status = EventReportStatus::WORKER_UNAVAILABLE;
        f.occupy(0, 2000);
        assert(f.boolean("alarm_active") && !f.boolean("event_accepted"));
        assert(submissions.size() == 1);
        f.frame(2500);
        f.frame(2999);
        assert(submissions.size() == 1);
        next_status = accepted;
        f.frame(3000);
        assert(submissions.size() == 2 && f.boolean("event_accepted"));
        f.occupy(3500, 5000);
        assert(submissions.size() == 2);
    }
}

void parameters_and_instances()
{
    Fixture a(0), b(1);
    assert(a.ctx.param_int("hold_ms") == 2000 && a.ctx.param_string("roi_name") == "alarm_zone");
    a.occupy(0, 1500);
    b.frame(1500);
    a.frame(2000);
    b.frame(2000);
    assert(a.boolean("alarm_active") && !b.boolean("alarm_active"));
    assert(submissions.size() == 1 && submissions.front().channel == 0);
    const auto old_state = a.state; // 保持旧分配存活，确保比较的不是重用地址。
    a.configure(R"({"hold_ms":1000})");
    a.frame(2500);
    assert(a.state != old_state && a.integer("dwell_ms") == 0);
    a.occupy(3000, 3500);
    assert(submissions.size() == 2 && a.boolean("alarm_active"));
    b.occupy(2500, 3500);
    assert(submissions.size() == 3 && submissions.back().channel == 1);
    const auto unchanged = a.state;
    a.configure(R"({"hold_ms":1000})");
    assert(a.state == unchanged);
    for (const auto &json : {R"({"hold_ms":-1})", R"({"hold_ms":1.5})", R"({"hold_ms":"1000"})", R"({"unknown":1})",
                             R"({"max_gap_ms":0})"})
    {
        std::vector<LogicParameterError> errors;
        assert(!logic_parameters_resolve(logic_name, json, nullptr, nullptr, &errors));
        assert(!errors.empty());
    }
    for (const auto &json :
         {R"({"target_label":"car"})", R"({"roi_name":"other"})", R"({"max_gap_ms":500})", R"({"hold_ms":0})"})
        assert(logic_parameters_reload_impact(logic_name, "{}", json) == LogicReloadImpact::RESET_STATE);
    a.configure(R"({"hold_ms":0,"target_label":"car"})");
    a.frame(4000); // 新标签不应继续计入旧 person 结果。
    assert(a.integer("target_count") == 0);
    a.frame(4500, {target(180, "car")});
    assert(a.boolean("alarm_active") && submissions.size() == 4);
}

void rendering(const std::string &directory)
{
    Fixture f;
    f.occupy(0, 2000);
    assert(text_overlay_available());
    RenderParams params;
    params.inputW = params.inputH = 640;
    params.show_fps = 0;
    params.show_system_overlays = false;
    params.draw_cmds = &f.drawing;
    for (auto size : {cv::Size(640, 640), cv::Size(1280, 720)})
        for (auto target : {DrawCommand::DISPLAY, DrawCommand::IMAGE, DrawCommand::VIDEO})
        {
            params.target_mask = target;
            cv::Mat image = cv::Mat::zeros(size, CV_8UC3);
            render_overlays(image, params);
            const auto pixel = image.at<cv::Vec3b>(size.height * 300 / 640, size.width * 100 / 640);
            assert(pixel[2] == 255 && pixel[1] == 0); // 红色区域线随画布缩放。
            assert(cv::countNonZero(image(cv::Rect(0, 0, size.width, size.height * 60 / 640)).reshape(1)) > 0);
            if (target == DrawCommand::IMAGE && size.width == 640)
                assert(cv::imwrite(directory + "/alarm.png", image));
        }
}

void replay(const std::string &directory, const std::string &video)
{
    cv::VideoCapture input(video, cv::CAP_FFMPEG);
    assert(input.isOpened());
    const double fps = input.get(cv::CAP_PROP_FPS);
    assert(fps >= 1 && fps <= 240);
    cv::VideoWriter output(directory + "/replay.avi", cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), fps, {640, 640});
    assert(output.isOpened());
    std::ofstream trace(directory + "/replay.csv");
    assert(trace);
    trace << "frame,time_ms,count,dwell_ms,alarm,event_accepted,submission_count\n";
    Fixture f;
    submissions.clear();
    int frames = 0;
    bool saved_alarm = false;
    for (; frames < static_cast<int>(std::ceil(fps * 8)); ++frames)
    {
        cv::Mat frame;
        assert(input.read(frame));
        cv::resize(frame, frame, {640, 640});
        const uint64_t now = static_cast<uint64_t>(std::llround(frames * 1000.0 / fps));
        // 真实录像画面 + 已知检测结果；不把可控输入冒充 NPU 检测。
        const bool present = (now >= 1000 && now < 4000) || now >= 5000;
        f.frame(now, present ? std::vector<AlgoResult>{target()} : std::vector<AlgoResult>{});
        RenderParams params;
        params.inputW = params.inputH = 640;
        params.show_fps = 0;
        params.show_system_overlays = false;
        params.draw_cmds = &f.drawing;
        render_overlays(frame, params);
        if (present)
            cv::rectangle(frame, target().box, {255, 200, 0}, 2);
        cv::putText(frame, "SDK TEST: SCRIPTED DETECTIONS", {20, 600}, cv::FONT_HERSHEY_SIMPLEX, .65, {0, 255, 255}, 2);
        output.write(frame);
        trace << frames << ',' << now << ',' << f.integer("target_count") << ',' << f.integer("dwell_ms") << ','
              << f.boolean("alarm_active") << ',' << f.boolean("event_accepted") << ',' << submissions.size() << '\n';
        if (f.boolean("alarm_active") && !saved_alarm)
        {
            assert(cv::imwrite(directory + "/replay_alarm.png", frame));
            saved_alarm = true;
        }
    }
    assert(submissions.size() == 2);
    std::cout << "Replay: " << frames << " decoded frames, " << fps << " FPS, 2 expected events\n";
}
} // namespace

// 只替换具有外部副作用的事件出口；SDK 参数/查询/状态/绘图/注册执行真实代码。
EventReportResult report_event(ChannelContext *ctx, const EventRequest &request)
{
    submissions.push_back({ctx->chnId, ctx->timestamp_ms, request, *VisionContextAccess::drawing(*ctx)});
    EventReportResult result;
    result.status = next_status;
    return result;
}
extern "C" int app_ctrl_get_performance_display()
{
    return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    cv::setNumThreads(1);
    for (auto test : {boundary_and_payload, missing_and_invalid_rois, interruptions, retry_and_acceptance,
                      parameters_and_instances})
    {
        submissions.clear();
        next_status = EventReportStatus::CREATED;
        test();
    }
    submissions.clear();
    next_status = EventReportStatus::CREATED;
    rendering(argv[1]);
    if (argc == 3)
        replay(argv[1], argv[2]);
    std::cout << "SDK workflow: timing, ROI, inference validity, retry, schema, isolation and rendering passed\n";
}
