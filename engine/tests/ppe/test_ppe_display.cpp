// Replay the registered PPE callback, including the event payload and the production renderer.
#include <algorithm>
#include <cassert>
#include <logic_session.h>
#include <rendering.h>

extern const char *logic_embedded_catalog_json();

static bool has_text(const std::vector<DrawCommand> &commands, const char *text)
{
    return std::any_of(commands.begin(), commands.end(), [text](const DrawCommand &command) {
        return command.type == DrawCommand::TEXT && command.text == text;
    });
}

int main()
{
    vision::set_logic_catalog(logic_embedded_catalog_json());
    vision::LogicSessionOptions options;
    options.channel.logic = "logic_ppe_guard";
    options.channel.logic_parameters_json = R"({"no_gloves_labels":"hand_bare","glove_positive_labels":"hand_gloved"})";
    vision::LogicSession session(options);
    std::vector<AlgoResult> detections(2);
    detections[0].label = "person";
    detections[0].track_id = 7;
    detections[0].box = {100, 100, 120, 240};
    detections[1].label = "hand_bare";
    detections[1].box = {140, 180, 20, 25};
    for (auto &result : detections)
        result.score = 0.95f;
    vision::LogicFrame frame;
    frame.results = &detections;
    for (int index = 0; index < 3; ++index)
    {
        frame.frame_id = index;
        frame.timestamp_ms = index * 550;
        for (auto &result : detections)
            result.frame_id = index;
        session.run(frame);
        if (index < 2)
        {
            assert(has_text(session.drawing(), "疑似违规核验中"));
            assert(has_text(session.drawing(), "当前人数 1"));
            assert(has_text(session.drawing(), "候选人数 1"));
            assert(has_text(session.drawing(), "报警人数 0"));
            assert(!has_text(session.drawing(), "未戴手套 1人"));
        }
    }
    assert(session.events().size() == 1);
    const auto &event = session.events().front();
    assert(has_text(event.drawing, "已确认违规"));
    assert(has_text(event.drawing, "报警人数 1"));
    assert(has_text(event.drawing, "未戴手套 1人"));
    const auto &fields = event.request.fields.values();
    assert(fields.at("regions").text() ==
           R"([{"part":"未戴手套","source":"detected","x":140,"y":180,"w":20,"h":25}])");
    assert(fields.at("frame_id").number() == 2 && fields.at("frame_width").number() == 640);
    assert(fields.at("frame_height").number() == 640 && fields.at("track_id").number() == 7);
    for (auto target : {DrawCommand::DISPLAY, DrawCommand::IMAGE, DrawCommand::VIDEO})
    {
        vision::OverlayOptions overlays;
        overlays.target_mask = target;
        overlays.inputW = overlays.inputH = 640;
        overlays.results = &detections;
        overlays.draw_cmds = target == DrawCommand::IMAGE ? &event.drawing : &session.drawing();
        overlays.show_fps = false;
        cv::Mat image = cv::Mat::zeros(640, 640, CV_8UC3);
        vision::render_overlays(image, overlays);
        assert(cv::countNonZero(image(cv::Rect(500, 0, 140, 60)).reshape(1)) > 0);
        assert(image.at<cv::Vec3b>(190, 140) == cv::Vec3b(0, 0, 255));
        assert(image.at<cv::Vec3b>(300, 100) == cv::Vec3b(0, 0, 0));
        if (target == DrawCommand::IMAGE)
        {
            cv::Mat source_image = cv::Mat::zeros(720, 1280, CV_8UC3);
            vision::render_overlays(source_image, overlays);
            // The hand's left edge x=140 maps to 280. The person's edge x=100 stays unmarked.
            assert(source_image.at<cv::Vec3b>(214, 280) == cv::Vec3b(0, 0, 255));
            assert(source_image.at<cv::Vec3b>(338, 200) == cv::Vec3b(0, 0, 0));
        }
    }
    detections.pop_back();
    frame.timestamp_ms += 100;
    session.run(frame);
    assert(session.events().empty());
    assert(has_text(session.drawing(), "违规部位待定位"));
    assert(has_text(session.drawing(), "候选人数 0"));
    assert(has_text(session.drawing(), "报警人数 1"));
    assert(std::none_of(session.drawing().begin(), session.drawing().end(),
                        [](const DrawCommand &cmd) { return cmd.type == DrawCommand::RECT; }));
    frame.timestamp_ms += 500;
    detections.clear();
    session.run(frame);
    assert(has_text(session.drawing(), "未检出人员"));
    assert(has_text(session.drawing(), "当前人数 0"));
    assert(has_text(session.drawing(), "报警人数 0"));

    frame.inference_valid = false;
    session.run(frame);
    assert(has_text(session.drawing(), "等待推理结果"));
    assert(!has_text(session.drawing(), "未检出人员"));
    frame.inference_enabled = false;
    session.run(frame);
    assert(has_text(session.drawing(), "检测已暂停"));

    // Two tracks, three kinds of violation. The overall count is per person.
    options.channel.logic_parameters_json =
        R"({"no_helmet_labels":"head","no_gloves_labels":"hand_bare","workwear_require_positive":true})";
    vision::LogicSession group(options);
    frame.inference_valid = frame.inference_enabled = true;
    const auto add = [&](const char *label, cv::Rect box, int track_id) {
        AlgoResult result;
        result.label = label;
        result.track_id = track_id;
        result.box = box;
        result.score = 0.95f;
        detections.push_back(result);
    };
    add("person", {100, 100, 120, 240}, 7);
    add("person", {410, 100, 120, 240}, 8);
    add("head", {140, 110, 30, 30}, -1);
    add("hand_bare", {120, 200, 20, 20}, -1);
    add("hand_bare", {190, 200, 20, 20}, -1);
    for (int i = 0; i < 3; ++i)
    {
        frame.frame_id = i;
        frame.timestamp_ms = 500 * i;
        for (auto &result : detections)
            result.frame_id = i;
        group.run(frame);
    }
    assert(has_text(group.drawing(), "报警人数 2"));
    assert(has_text(group.drawing(), "未戴安全帽 1人"));
    assert(has_text(group.drawing(), "工装异常 2人"));
    assert(has_text(group.drawing(), "未戴手套 1人"));
}
