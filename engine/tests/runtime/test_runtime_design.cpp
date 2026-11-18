// Default: real pipeline workers with synthetic input, without camera/RKNN/GPIO calls.
// Optional --video CONFIG: exercise a callback failure through a real local-video capture.
#include "application/runtime/app_ctrl.h"
#include "application/runtime/runtime_status.h"
#include "media/capture/decode_channel.h"
#include "pipeline/pipeline_internal.h"
#include "pipeline/pipeline_runtime.h"
#include "vision/inference/inference_engine.h"
#include "vision/inference/inference_internal.h"
#include "vision/inference/npu_device_owner.h"
#include <global.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unistd.h>

namespace
{
void no_op(ChannelContext *)
{
}
std::atomic<int64_t> observed_video_time{-1};
void throws_standard(ChannelContext *ctx)
{
    observed_video_time.store(ctx->video_time_ms);
    throw std::runtime_error("intentional channel failure");
}
void throws_unknown(ChannelContext *ctx)
{
    observed_video_time.store(ctx->video_time_ms);
    throw 42;
}
vision::VideoRuntime *observed_runtime = nullptr;
void throws_global(GlobalContext *)
{
    // Public queries are also safe from a callback, while run() is active.
    assert(observed_runtime->is_running());
    assert(observed_runtime->channel_status().size() == 1);
    throw std::runtime_error("intentional global failure");
}

struct Fixture
{
    std::string directory, config, source;
    Fixture(const char *logic, bool global = false)
    {
        char path[] = "/tmp/vision-runtime-design-XXXXXX";
        assert(mkdtemp(path));
        directory = path;
        config = directory + "/config.json";
        source = directory + "/empty.mp4";
        std::ofstream(source).close();
        std::ofstream file(config);
        file << R"({"global":{"enable_display":false,"enable_rtsp":false,"tracker_enable":0)";
        if (global)
            file
                << R"(,"global_logics":[{"instance_id":"failure_test","enable":true,"logic":"throws_global","channels":[0],"logic_parameters":{}}])";
        file
            << R"(},"channels":[{"id":0,"enable":true,"infer_enable":false,"models":[],"stream":{"src_type":"file","url":")"
            << source << R"("},"logic":")" << logic
            << R"(","logic_parameters":{},"report_policy":{"enabled":false,"deliveries":[]}}]})";
        file.close();
        assert(file.good());
    }
    ~Fixture()
    {
        unlink(config.c_str());
        unlink(source.c_str());
        rmdir(directory.c_str());
    }
};

void source_membership()
{
    static_assert(std::is_const<decltype(GstChannel_t::chnIds)>::value, "capture membership must be immutable");
    SrcCfg_t a;
    a.srcType = "rtsp";
    a.location = "rtsp://127.0.0.1:1/test";
    a.videoEncType = "h264";
    auto b = a;
    assert(capture_sources_share(a, b));
    b.videoEncType = "h265";
    assert(!capture_sources_share(a, b));
    a.srcType = b.srcType = "file";
    assert(!capture_sources_share(a, b));
    a.srcType = b.srcType = "usb";
    b.usb_width = 1280;
    b.usb_height = 720;
    assert(capture_sources_share(a, b));
    b.usb_width = 640;
    assert(!capture_sources_share(a, b));
    DecChannel capture({0, 3, 7}, a);
    assert(capture.channelId() == 0 && capture.hasChannel(7) && !capture.hasChannel(1));
    assert(capture.channelIds() == std::vector<int>({0, 3, 7}));
    for (const auto &ids : {std::vector<int>{}, std::vector<int>{0, 0}, std::vector<int>{-1}})
    {
        bool rejected = false;
        try
        {
            DecChannel invalid(ids, a);
        }
        catch (const std::invalid_argument &)
        {
            rejected = true;
        }
        assert(rejected);
    }
}

void device_ownership()
{
    AppConfig config;
    config.enable_display = config.enable_rtsp = false;
    // A pure video pipeline must neither resume nor stop another session's gate.
    npu::CoreLease::resume();
    {
        npu::CoreLease other_session(1);
        assert(inference_init(config) == 0);
        inference_deinit();
        other_session.check_health();
    }
    npu::CoreLease::stop();
    assert(inference_init(config) == 0);
    bool still_stopped = false;
    try
    {
        npu::CoreLease lease(1);
    }
    catch (const npu::Cancelled &)
    {
        still_stopped = true;
    }
    assert(still_stopped);
    inference_deinit();
    npu::CoreLease::resume();

    // The test holds the normal lock, or respects an existing owner. It never unlinks it.
    std::unique_ptr<npu::DeviceOwner> holder;
    try
    {
        holder.reset(new npu::DeviceOwner());
    }
    catch (const std::runtime_error &error)
    {
        assert(std::string(error.what()).find("owned by another") != std::string::npos);
    }
    assert(inference_init(config) == 0); // No models must not acquire the occupied lock.
    inference_deinit();
    ChannelConfig channel;
    channel.id = 0;
    channel.enable = channel.infer_enable = true;
    ChannelModelConfig model;
    model.enable = true;
    model.model_type = "yolov8_det";
    model.model_path = "/unused-test-model.rknn";
    channel.models.push_back(model);
    config.channels.push_back(channel);
    bool rejected = false;
    try
    {
        inference_init(config);
    }
    catch (const std::runtime_error &error)
    {
        rejected = std::string(error.what()).find("owned by another") != std::string::npos;
    }
    assert(rejected); // Ownership is checked BEFORE touching RKNN or the model file.
    inference_deinit();

    config.channels.clear();
    assert(inference_init(config) == 0); // Rollback left initialization usable.
    // Existing dormant topology: re-enabling inference must reacquire the lock.
    g_inference.model_groups_per_chn[0].resize(1);
    auto queue = std::make_unique<InferenceTaskQueue>();
    pthread_mutex_init(&queue->mtx, nullptr);
    pthread_cond_init(&queue->cv, nullptr);
    g_inference.task_queues.push_back(std::move(queue));
    g_inference.worker_tids.resize(1);
    g_inference.worker_started.assign(1, 0);
    channel.threads = 1;
    assert(!inference_reload_channel_model(0, channel, npu::AutoCorePlacement{}));
    assert(!g_inference.model_groups_per_chn[0][0] && !g_inference.worker_started[0]);
    channel.infer_enable = false;
    assert(inference_reload_channel_model(0, channel, npu::AutoCorePlacement{}));
    assert(g_inference.model_groups_per_chn[0].size() == 1 && !g_inference.model_groups_per_chn[0][0]);
    assert(!g_inference.worker_started[0]); // No worker may construct FrameExecutor with a null model.
    inference_deinit();
}

void status_transitions()
{
    Fixture fixture("no_op");
    assert(app_ctrl_init(fixture.config.c_str()) == 0);
    assert(pipeline_init() == 0);
    auto status = app_ctrl_channel_status().at(0);
    assert(status.state == vision::VideoChannelState::Connecting && status.received_frames == 0);
    assert(status.last_frame_age_ms == -1);
    std::vector<char> pixels(64 * 64 * 3 / 2, 0);
    FrameInputDesc frame{};
    frame.chnId = 0;
    frame.width = frame.height = frame.horStride = frame.verStride = 64;
    frame.fd = -1;
    frame.dataSize = pixels.size();
    std::strcpy(frame.fmt, "NV12");
    pipeline_submit_frame(pixels.data(), frame);
    status = app_ctrl_channel_status().at(0);
    assert(status.state == vision::VideoChannelState::Streaming && status.received_frames == 1);
    assert(status.width == 64 && status.height == 64 && status.last_frame_age_ms >= 0);
    app_ctrl_capture_status(0, vision::VideoChannelState::Disconnected, "test disconnection");
    pipeline_channel_offline(0);
    status = app_ctrl_channel_status().at(0);
    assert(status.state == vision::VideoChannelState::Disconnected && status.error == "test disconnection");
    pipeline_channel_online(0);
    assert(app_ctrl_channel_status().at(0).state == vision::VideoChannelState::Connecting);
    pipeline_submit_frame(pixels.data(), frame);
    status = app_ctrl_channel_status().at(0);
    assert(status.state == vision::VideoChannelState::Streaming && status.error.empty());
    assert(status.received_frames == 2);
    pipeline_deinit();
    pipeline_destroy_display_queues();
    app_ctrl_deinit();
}

void worker_failure(const char *logic, bool dispatch)
{
    observed_video_time.store(-1);
    Fixture fixture(logic);
    assert(app_ctrl_init(fixture.config.c_str()) == 0);
    assert(pipeline_init() == 0);
    ChannelRawFrame raw;
    raw.width = raw.height = 64;
    raw.frame_steady_ms = steady_now_ms();
    raw.video_time_ms = 1250;
    raw.lazy_frame = std::make_shared<LazyVideoFrame>(0, nullptr, 64, 64, 64, 64, RK_FORMAT_YCbCr_420_SP, 640, 640);
    if (dispatch)
    {
        // Inject only the dispatch input; no NPU model is created by this test.
        auto &channel = g_pCtrl->config.channels[0];
        channel.infer_enable = true;
        ChannelModelConfig model;
        model.enable = true;
        model.model_type = "yolov8_det";
        model.model_path = "/unused-test-model.rknn";
        channel.models.push_back(model);
        assert(pipeline_publish_runtime_snapshot(g_pCtrl->config, 2));
        auto &result = g_inference.channel_results[0];
        result.frame = raw.lazy_frame;
        result.latest_seq = 1;
        result.frame_steady_ms = raw.frame_steady_ms;
        result.video_time_ms = raw.video_time_ms; // 无检测框时也必须保留对应帧的位置。
        result.has_new = 1;
        g_inference.result_dispatch_pending[0] = 1;
    }
    else
        traditional_logic_publish(0, raw, 1);
    pthread_t worker;
    assert(pthread_create(&worker, nullptr, dispatch ? pipeline_dispatch_worker : pipeline_logic_worker, nullptr) == 0);
    for (int i = 0; i < 200 && g_pCtrl->isRunning.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const bool stopped = !g_pCtrl->isRunning.load();
    pipeline_request_stop();
    pthread_join(worker, nullptr);
    assert(stopped && g_pCtrl->worker_failed);
    assert(observed_video_time.load() == 1250);
    assert(pthread_mutex_trylock(&g_process_mtx[0]) == 0); // Exception released the process lock.
    pthread_mutex_unlock(&g_process_mtx[0]);
    const auto status = app_ctrl_channel_status().at(0);
    assert(status.state == vision::VideoChannelState::Failed && !status.error.empty());
    assert(g_pCtrl->channels_state[0].published_frame_seq == 0); // No partial callback result published.
    pipeline_deinit();
    pipeline_destroy_display_queues();
    app_ctrl_deinit();
}

void runtime_failure()
{
    Fixture fixture("no_op", true);
    vision::VideoRuntimeOptions options;
    options.config_path = fixture.config;
    vision::VideoRuntime runtime(options);
    observed_runtime = &runtime;
    assert(runtime.channel_status().empty() && runtime.last_error().empty());
    std::atomic<bool> done{false};
    std::thread observer([&] {
        for (int i = 0; i < 5000 && !done.load(); ++i)
        {
            runtime.channel_status(); // Concurrent query during initialization and teardown.
            runtime.last_error();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!done.load())
            runtime.request_stop();
    });
    const int result = runtime.run();
    done.store(true);
    observer.join();
    assert(result == 7 && !runtime.is_running());
    assert(runtime.last_error().find("intentional global failure") != std::string::npos);
    assert(runtime.channel_status().at(0).state == vision::VideoChannelState::Stopped);
    assert(g_pCtrl == nullptr);
}
} // namespace

int main(int argc, char **argv)
{
    // Keep test services and output independent from a surrounding service environment.
    for (const char *name : {"NOTIFY_SOCKET", "WATCHDOG_USEC", "WATCHDOG_PID", "RK_SHUTDOWN_STATUS_FILE",
                             "RK_LOGIC_CONTROL_SOCKET", "RK_FILE_PLAYBACK_CONTROL_SOCKET", "RK_DATASET_SOCKET"})
        unsetenv(name);
    register_logic("no_op", no_op);
    register_logic("throws_standard", throws_standard);
    register_logic("throws_unknown", throws_unknown);
    register_global_logic("throws_global", throws_global);
    if (argc == 3 && std::strcmp(argv[1], "--video") == 0)
    {
        vision::VideoRuntimeOptions options;
        options.config_path = argv[2];
        vision::VideoRuntime runtime(options);
        assert(runtime.run() == 7);
        const auto status = runtime.channel_status().at(0);
        assert(status.state == vision::VideoChannelState::Failed);
        assert(status.received_frames > 0 && status.width == 320 && status.height == 240);
        assert(status.error.find("intentional channel failure") != std::string::npos);
        assert(runtime.last_error().find("intentional channel failure") != std::string::npos);
        assert(g_pCtrl == nullptr);
        std::puts("real-video callback exception: clean shutdown and retained channel error passed");
        return 0;
    }
    source_membership();
    device_ownership();
    status_transitions();
    worker_failure("throws_standard", false);
    worker_failure("throws_unknown", false);
    worker_failure("throws_standard", true);
    runtime_failure();
    std::puts("runtime source ownership, NPU ownership, status and exception regressions passed");
}
