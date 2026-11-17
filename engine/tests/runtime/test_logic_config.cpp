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
    bool accepts(const char *channel_logic, const char *global_logic = nullptr)
    {
        std::ofstream file(path);
        file << R"({"global":{"enable_display":false,"enable_rtsp":false,"tracker_enable":0)";
        if (global_logic)
            file << R"(,"global_logics":[{"instance_id":"test","enable":true,"logic":")" << global_logic
                 << R"(","channels":[0],"logic_parameters":{}}])";
        file
            << R"(},"channels":[{"id":0,"enable":true,"infer_enable":false,"models":[],"stream":{"src_type":"file","url":")"
            << source << R"("},"logic":")" << channel_logic
            << R"(","logic_parameters":{},"report_policy":{"enabled":false,"deliveries":[]}}]})";
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
    assert(fixture.accepts("channel_only", "global_only"));
    assert(fixture.accepts("")); // An empty channel logic deliberately disables business processing.
    assert(!fixture.accepts("global_only"));
    assert(!fixture.accepts("", "channel_only"));
    assert(!fixture.accepts("missing_channel")); // A schema alone does not register callable code.
    assert(!fixture.accepts("", "missing_global"));
    assert(!fixture.accepts("", ""));
    assert(fixture.accepts("unlisted_channel") == !explicit_catalog);
    assert(fixture.accepts("channel_only", "global_only")); // Invalid files do not poison later loads.
    std::puts(explicit_catalog ? "explicit catalog: runtime callback kinds validated"
                               : "default catalog: runtime callback kinds validated");
}
