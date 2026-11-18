#include <atomic>
#include <cassert>
#include <channel.h>
#include <chrono>
#include <fstream>
#include <functional>
#include <global.h>
#include <iostream>
#include <runtime.h>
#include <thread>
#include <unistd.h>

namespace
{
struct Seen
{
    std::atomic<int> calls{0}, state_calls{0};
    std::atomic<int> inference_calls{0};
    std::atomic<uint64_t> timestamp{0}, real{0};
    std::atomic<int64_t> video{-1};
    std::atomic<int> dt{0};
} seen[2];
struct State
{
    int calls = 0;
};
std::atomic<bool> hold_frame{false}, frame_waiting{false};
void observe(ChannelContext *ctx)
{
    if (ctx->chnId == 0 && hold_frame)
    {
        frame_waiting = true;
        while (hold_frame)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(ctx->video_time_ms >= 0);
    assert(ctx->timestamp_ms == static_cast<uint64_t>(ctx->video_time_ms) + 1);
    if (ctx->results)
        for (const auto &result : *ctx->results)
            assert(result.timestamp_ms == ctx->timestamp_ms);
    auto &out = seen[ctx->chnId];
    if (ctx->infer_enabled && ctx->inference_valid)
        ++out.inference_calls;
    out.timestamp = ctx->timestamp_ms;
    out.real = ctx->real_time_ms;
    out.video = ctx->video_time_ms;
    out.dt = static_cast<int>(ctx->dt_ms);
    out.state_calls = ++ctx->get_state<State>()->calls;
    ++out.calls;
}
std::atomic<int> global_calls{0};
std::atomic<bool> expect_global_input{false};
void observe_global(GlobalContext *ctx)
{
    if (expect_global_input)
    {
        assert(ctx->input(0));
        assert(ctx->dt_ms < 1000);
    }
    ++global_calls;
}
void wait_for(const std::function<bool()> &ready)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!ready() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(ready());
}
vision::FilePlaybackStatus status(vision::VideoRuntime &runtime, int id)
{
    for (const auto &file : runtime.file_playback())
        if (file.channel_id == id)
            return file;
    return {};
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 4);
    register_logic("observe", observe);
    register_global_logic("observe_global", observe_global);
    char path[] = "/tmp/file-playback-test-XXXXXX";
    assert(mkdtemp(path));
    const std::string config = std::string(path) + "/config.json";
    {
        std::ofstream out(config);
        out << R"({"global":{"enable_display":false,"enable_rtsp":false,"tracker_enable":0,"global_logics":[{"instance_id":"observe","logic":"observe_global","enable":true,"channels":[0],"poll_interval_ms":20,"logic_parameters":{}}]},"channels":[)";
        for (int id = 0; id < 2; ++id)
        {
            if (id)
                out << ',';
            out << R"({"id":)" << id << R"(,"enable":true,"max_fps":20,"models":[)";
            if (argc == 4 && id == 0)
                out << R"({"id":"detector","enable":true,"model_type":"yolov8_det","model_path":")" << argv[2]
                    << R"(","label_path":")" << argv[3] << R"("})";
            out << R"(],"stream":{"src_type":"file","url":")" << argv[1]
                << R"("},"logic":"observe","logic_parameters":{},"report_policy":{"enabled":false,"deliveries":[]}})";
        }
        out << "]}";
    }
    vision::VideoRuntimeOptions options;
    options.config_path = config;
    vision::VideoRuntime runtime(options);
    assert(!runtime.set_file_paused(0, true));
    std::atomic<int> exit_code{-99};
    std::thread running([&] { exit_code = runtime.run(); });
    wait_for([&] { return status(runtime, 0).available && seen[0].calls >= 3 && seen[1].calls >= 3; });
    assert((seen[0].inference_calls > 0) == (argc == 4));
    assert(seen[1].inference_calls == 0); // No model: video and logic still run.
    assert(!runtime.seek_file(0, -1));
    assert(!runtime.set_file_paused(17, true));
    const auto pause_begin = std::chrono::steady_clock::now();
    assert(runtime.set_file_paused(0, true));
    wait_for([&] {
        auto s = status(runtime, 0);
        return s.paused && !s.busy;
    });
    const auto pause_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - pause_begin).count();
    std::cout << "pause acknowledgement: " << pause_ms << " ms\n";
    const int calls = seen[0].calls;
    const int state_calls = seen[0].state_calls;
    const auto real = seen[0].real.load();
    const auto video = seen[0].video.load();
    const auto position = status(runtime, 0).position_ms;
    const int other_calls = seen[1].calls;
    // 超过原来的 3 秒文件无帧看门狗，仍应保留同一业务状态。
    std::this_thread::sleep_for(std::chrono::milliseconds(3300));
    assert(seen[0].calls == calls && seen[0].video == video);
    assert(status(runtime, 0).position_ms == position);
    assert(seen[1].calls > other_calls + 10);
    const int joint_calls = global_calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    assert(global_calls == joint_calls);
    expect_global_input = true;
    assert(runtime.set_file_paused(0, false));
    wait_for([&] { return !status(runtime, 0).busy && seen[0].calls > calls; });
    assert(seen[0].real > real + 3000);
    assert(seen[0].video < video + 1000);
    assert(seen[0].dt < 1000 && seen[0].state_calls > state_calls);
    wait_for([&] { return global_calls > joint_calls; });
    expect_global_input = false;
    assert(runtime.set_file_paused(0, true));
    wait_for([&] {
        auto s = status(runtime, 0);
        return s.paused && !s.busy;
    });
    for (int64_t target : {int64_t(2000), int64_t(0)})
    {
        const int before = seen[0].calls;
        assert(runtime.seek_file(0, target));
        wait_for([&] {
            auto s = status(runtime, 0);
            return s.paused && !s.busy && seen[0].calls > before;
        });
        assert(seen[0].calls == before + 1 && seen[0].state_calls == 1);
        assert(seen[0].video >= target && seen[0].video < target + 100);
        assert(seen[0].dt == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        assert(seen[0].calls == before + 1);
    }
    assert(runtime.set_file_paused(0, false));
    wait_for([&] { return seen[0].state_calls > 1; });
    assert(seen[0].dt > 0 && seen[0].dt < 1000); // 不重复分析 seek 的预览帧。
    // 尚在执行的业务不能被截断；完成后直接唤醒暂停确认。
    hold_frame = true;
    wait_for([&] { return frame_waiting.load(); });
    assert(runtime.set_file_paused(0, true));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    assert(status(runtime, 0).busy && !status(runtime, 0).paused);
    const auto drain_begin = std::chrono::steady_clock::now();
    hold_frame = false;
    wait_for([&] { return status(runtime, 0).paused; });
    std::cout
        << "last analysis to pause acknowledgement: "
        << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - drain_begin).count()
        << " ms\n";
    runtime.request_stop();
    running.join();
    assert(exit_code == 0);
    unlink(config.c_str());
    rmdir(path);
    std::cout << "file pause/resume, clocks, two channels, global input, seek and paused shutdown passed\n";
}
