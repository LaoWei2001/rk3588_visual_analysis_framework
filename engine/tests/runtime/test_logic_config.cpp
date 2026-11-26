// Validate real runtime configuration without opening a source, NPU or control service.
#include "application/config/config.h"
#include <channel.h>
#include <global.h>
#include <parameters.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

namespace
{
void channel_only(ChannelContext *)
{
}
void unlisted_channel(ChannelContext *)
{
}
void global_only(GlobalContext *)
{
}

struct ConfigFixture
{
    std::string directory, source, path;
    ConfigFixture()
    {
        char name[] = "/tmp/vision-logic-config-XXXXXX";
        const char *created = mkdtemp(name);
        assert(created);
        directory = created;
        source = directory + "/source.mp4";
        path = directory + "/config.json";
        std::ofstream(source).close();
    }
    ~ConfigFixture()
    {
        unlink(path.c_str());
        unlink(source.c_str());
        rmdir(directory.c_str());
    }
    bool accepts(const char *channel_logic, const char *global_logic = nullptr,
                 const char *policy = R"({"enabled":false,"deliveries":[]})", const char *channel_extra = "", const char *global_extra = "")
    {
        std::ofstream file(path);
        file << R"({"global":{"enable_display":false,"enable_rtsp":false,"tracker_enable":0)" << global_extra;
        if (global_logic)
            file << R"(,"global_logics":[{"instance_id":"test","enable":true,"logic":")" << global_logic
                 << R"(","channels":[0],"logic_parameters":{},"report_policy":)" << policy << "}]";
        file
            << R"(},"channels":[{"id":0,"enable":true,"models":[],"stream":{"src_type":"file","url":")"
            << source << R"("},"logic":")" << channel_logic
            << R"(","logic_parameters":{},"report_policy":)" << policy << channel_extra << "}]}";
        file.close();
        assert(file.good());
        AppConfig config;
        return load_config(path.c_str(), config);
    }
};
} // namespace

int main(int argc, char **)
{
    register_logic("channel_only", channel_only);
    register_logic("unlisted_channel", unlisted_channel);
    register_global_logic("global_only", global_only);
    const bool explicit_catalog = argc > 1;
    if (explicit_catalog)
        vision::set_logic_catalog(R"({
          "channel_logics":[
            {"name":"channel_only","parameters":{"properties":{}}},
            {"name":"missing_channel","parameters":{"properties":{}}}],
          "global_logics":[
            {"name":"global_only","parameters":{"properties":{}}},
            {"name":"missing_global","parameters":{"properties":{}}}]
        })");

    ConfigFixture fixture;
    const std::string source_prefix = R"({"global":{},"channels":[{"id":0,"stream":{"src_type":"rtsp","url":")";
    for (const bool reload : {false, true})
    {
        for (const std::string &address : std::vector<std::string>{
                 "rtsp://camera/live", "rtsps://camera/live", "http://camera/video", "https://camera/video",
                 fixture.source, "ftp://camera/video", "relative.mp4"})
        {
            std::ofstream(fixture.path) << source_prefix << address << R"("}}]})";
            AppConfig config;
            if (reload)
                config.config_path = "HOTRELOAD";
            const bool valid = address != "ftp://camera/video" && address != "relative.mp4";
            assert(load_config(fixture.path, config) == valid);
            if (valid)
                assert(config.channels.at(0).stream.src_type == (address == fixture.source ? "file" : "rtsp"));
        }
        for (int invalid_id : {0, -1, 9999})
        {
            std::ofstream(fixture.path) << source_prefix << fixture.source << R"("}},{"id":)" << invalid_id
                                        << R"(,"stream":{"src_type":"file","url":")" << fixture.source << R"("}}]})";
            AppConfig config;
            if (reload)
                config.config_path = "HOTRELOAD";
            assert(!load_config(fixture.path, config));
        }
    }
    for (const char *removed : {R"(,"rtsp_codec":"h265")", R"(,"rtsp_encoder":"hw")", R"(,"enable_pause_key":true)"})
        assert(!fixture.accepts("channel_only", nullptr, "{}", "", removed));
    // There is no hidden per-channel override: each model node controls itself.
    ChannelConfig channel;
    assert(!config_utils::is_channel_infer_enabled(channel));
    ChannelModelConfig model;
    model.id = "detector";
    model.model_type = "yolov8_det";
    model.model_path = "assets/model.rknn";
    model.enable = false;
    channel.models.push_back(model);
    assert(!config_utils::is_channel_infer_enabled(channel));
    channel.models[0].enable = true;
    assert(config_utils::is_channel_infer_enabled(channel));
    channel.models.push_back(model);
    channel.models[0].enable = false;
    channel.models[1].enable = true;
    assert(config_utils::is_channel_infer_enabled(channel));
    channel.models[1].enable = false;
    assert(!config_utils::is_channel_infer_enabled(channel));
    for (const char *obsolete : {R"(,"infer_enable":false)", R"(,"infer_enable":true)"})
        assert(!fixture.accepts("channel_only", nullptr, "{}", obsolete));
    for (const char *obsolete : {R"(,"swap_rb":false)", R"(,"swap_rb":true)", R"(,"report_parameters":{})"})
        assert(!fixture.accepts("channel_only", nullptr, "{}", obsolete));
    assert(fixture.accepts("channel_only", "global_only"));
    assert(fixture.accepts("")); // An empty channel logic deliberately disables business processing.
    assert(!fixture.accepts("global_only"));
    assert(!fixture.accepts("", "channel_only"));
    assert(!fixture.accepts("missing_channel")); // A schema alone does not register callable code.
    assert(!fixture.accepts("", "missing_global"));
    assert(!fixture.accepts("", ""));
    assert(fixture.accepts("unlisted_channel") == !explicit_catalog);
    for (const char *policy : {R"({"parameters":[]})", R"({"deliveries":[{"mapping":[]}]})",
                               R"({"image_selection":{"old_option":true}})",
                               R"({"deliveries":[{"when":{"old_option":true}}]})",
                               R"({"deliveries":{}})"})
    {
        assert(!fixture.accepts("channel_only", nullptr, policy));
        assert(!fixture.accepts("channel_only", "global_only", policy));
    }
    assert(fixture.accepts("channel_only", "global_only",
                          R"({"enabled":false,"deliveries":[{"id":"d","enabled":false,"contract_label":"label","when":{"event_types":["alarm"]}}],"image_selection":{"mode":"connected"},"merge_window_sec":5})"));
    assert(fixture.accepts("channel_only", "global_only")); // Invalid files do not poison later loads.
    // Reloading a smaller file must reset every omitted global setting.
    assert(fixture.accepts("channel_only", nullptr, "{}", R"(,"tracker_enable":-1)"));
    AppConfig reused;
    reused.performance_display = false;
    reused.rtsp_port = 9554;
    reused.rtsp_path = "/old";
    reused.rtsp_bitrate = 1234;
    assert(load_config(fixture.path, reused));
    const AppConfig defaults;
    assert(reused.performance_display == defaults.performance_display);
    assert(reused.rtsp_port == defaults.rtsp_port);
    assert(reused.rtsp_path == defaults.rtsp_path);
    assert(reused.rtsp_bitrate == defaults.rtsp_bitrate);
    assert(reused.channels.size() == 1 && reused.channels[0].tracker_enable == 0);
    assert(!reused.channels[0].stream.loop);
    // Numeric and boolean switch encodings agree with the canvas.
    {
        std::ofstream file(fixture.path);
        file << R"({"global":{"enable_display":false,"tracker_enable":false,
          "global_logics":[{"instance_id":"numeric","enable":1,"logic":"global_only"}]},
          "channels":[{"id":0,"stream":{"src_type":"file","loop":1,"url":")"
             << fixture.source << R"("},"models":[{"id":"off","enable":0}],"tracker_enable":false}]})";
    }
    assert(load_config(fixture.path, reused));
    assert(reused.tracker_enable == 0 && reused.channels[0].tracker_enable == 0);
    assert(reused.global_logics[0].enable && reused.channels[0].stream.loop);
    assert(!reused.channels[0].models[0].enable);
    std::puts(explicit_catalog ? "explicit catalog: runtime callback kinds validated"
                               : "default catalog: runtime callback kinds validated");
}
