#include "alarm/alarm_report.h"
#include "config/config.h"
#include "logic/core/channel_logic.h"
#include "recorder/event_video_recorder.h"
#include "player/display.h"
#include "third_party/json/cJSON.h"

#include <opencv2/opencv.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>

/* alarm_report.cpp 的边界依赖测试桩：测试复用主程序真实告警/manifest/合并逻辑，
 * 只隔离显示渲染和录像线程。 */
RenderParams ChannelContext::render_params(int64_t) const { return RenderParams{}; }
std::string ChannelContext::time_str() const { return "2023-11-14 22:13:20"; }
void render_overlays(cv::Mat &, const RenderParams &) {}
int event_video_recorder_trigger(const EventVideoRequest &) { return 1; }
void event_video_recorder_extend(int, const std::string &) {}
void event_video_recorder_push_source_frame(int, const void *, int, int, int, int, int, int, float,
                                            EventVideoOverlayMode) {}
void event_video_recorder_deinit(void) {}

static bool file_contains(const std::filesystem::path &path, const std::string &text)
{
    std::ifstream in(path);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return all.find(text) != std::string::npos;
}

static int json_int(const std::filesystem::path &path, const char *key, int fallback = -1)
{
    std::ifstream in(path);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    cJSON *root = cJSON_Parse(all.c_str());
    if (!root) return fallback;
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    const int value = cJSON_IsNumber(item) ? item->valueint : fallback;
    cJSON_Delete(root);
    return value;
}

int main()
{
    namespace fs = std::filesystem;
    const fs::path outbox = fs::temp_directory_path() / "rk3588_alarm_report_unit";
    fs::remove_all(outbox);
    fs::create_directories(outbox);
    setenv("ALARM_STORE_DIR", outbox.c_str(), 1);

    ChannelConfig config;
    config.report_policy_json = R"({"merge_window_sec":5,"deliveries":[{"id":"image_server","enabled":true,"media":"image","target":"server","inputs":[]},{"id":"video_dify","enabled":true,"media":"video","target":"dify","inputs":[]}]})";
    config.report_parameters_json = R"({"production_line":"A"})";

    cv::Mat frame(64, 96, CV_8UC3, cv::Scalar(20, 40, 60));
    ChannelContext ctx{};
    ctx.chnId = 2;
    ctx.frame = &frame;
    ctx.config = &config;
    ctx.unix_ms = 1700000000000ULL;

    AlarmRequest first;
    first.type = "unit_alarm";
    first.message = "first";
    first.fields.set_string("step", "wash");
    first.fields.set_number("score", 0.91);

    const std::string id1 = alarm_report(&ctx, first);
    const std::string id2 = alarm_report(&ctx, first);

    AlarmRequest other = first;
    other.type = "other_alarm";
    const std::string id3 = alarm_report(&ctx, other);

    alarm_report_deinit();

    bool ok = true;
    ok = ok && !id1.empty() && id1 == id2;        // 同通道同类型5秒内合并
    ok = ok && !id3.empty() && id3 != id1;        // 不同报警类型不合并
    ok = ok && fs::is_regular_file(outbox / id1 / "manifest.json");
    ok = ok && fs::is_regular_file(outbox / id1 / "snapshot.jpg");
    ok = ok && file_contains(outbox / id1 / "manifest.json", "unit_alarm");
    ok = ok && file_contains(outbox / id1 / "manifest.json", "production_line");
    ok = ok && file_contains(outbox / id1 / "manifest.json", "image_server");
    ok = ok && file_contains(outbox / id1 / "manifest.json", "video_dify");
    ok = ok && json_int(outbox / id1 / "manifest.json", "trigger_count") == 2;

    std::cout << (ok ? "alarm_report_unit_test: PASS\n" : "alarm_report_unit_test: FAIL\n");
    fs::remove_all(outbox);
    return ok ? 0 : 1;
}
