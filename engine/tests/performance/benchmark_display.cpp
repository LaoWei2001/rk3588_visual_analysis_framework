// RK3588 板端绘制微基准：使用真实 RGA 和显示代码，不运行采集、推理或 RTSP。
// 配置/推理 FPS 查询为固定桩；输出 JSON 行，耗时不代表完整视频管线。
#include "display/display_pipeline.h"
#include "pipeline/frame_transform.h"
#include "runtime/app_ctrl.h"
#include "runtime/pause_ctrl.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <vector>

APP_CTRL *g_pCtrl = nullptr;
static auto runtime = std::make_shared<AppRuntimeSnapshot>();
static int show_performance = 1;
int app_ctrl_get_performance_display()
{
    return show_performance;
}
int app_ctrl_has_channel(int id)
{
    return id >= 0 && id < 4;
}
int app_ctrl_get_channel_display_order(int id)
{
    return id;
}
int app_ctrl_get_disp_width()
{
    return 1920;
}
int app_ctrl_get_disp_height()
{
    return 1280;
}
int app_ctrl_get_tile_cols()
{
    return 2;
}
int app_ctrl_get_tile_rows()
{
    return 2;
}
float app_ctrl_get_disp_fps(int id)
{
    return g_pCtrl->channels_state[id].preview_rate.value();
}
std::shared_ptr<const AppRuntimeSnapshot> app_ctrl_get_runtime_snapshot()
{
    return runtime;
}
const ChannelConfig *app_ctrl_runtime_channel_config(const std::shared_ptr<const AppRuntimeSnapshot> &, int id)
{
    return &runtime->config.channels[id];
}
const std::vector<RoiZone> *app_ctrl_runtime_channel_rois(const std::shared_ptr<const AppRuntimeSnapshot> &, int id)
{
    return &runtime->roi_zones[id];
}
float inference_get_infer_fps(int)
{
    return 25.0f;
}
using Clock = std::chrono::steady_clock;

int main(int argc, char **argv)
{
    const int iterations = argc > 1 ? std::stoi(argv[1]) : 240;
    if (iterations <= 0)
    {
        std::cerr << "iterations must be positive\n";
        return 2;
    }
    cv::setNumThreads(1);
    g_pCtrl = new APP_CTRL();
    g_pCtrl->inputW = 640;
    g_pCtrl->inputH = 640;
    runtime->config.channels.resize(4);
    pause_ctrl::init(true);
    cv::Mat front(1280, 1920, CV_8UC3, cv::Scalar(0));
    char *front_data = reinterpret_cast<char *>(front.data);
    g_pCtrl->pDispBuffer = &front_data;
    // 与本地视频相同的源尺寸和布局；使用真 RGA 缩放，真实文字/框绘制和
    // framebuffer 写入。
    cv::Mat source(720, 1280, CV_8UC3, cv::Scalar(11, 22, 33));
    for (int id = 0; id < 4; ++id)
    {
        pthread_mutex_init(&g_pCtrl->chn_mtx[id], nullptr);
        for (int n = 0; n < 5; ++n)
        {
            AlgoResult r;
            r.box = cv::Rect(40 + n * 90, 80 + n * 60, 80, 100);
            r.label = "target";
            r.score = .85f;
            g_pCtrl->channels_state[id].last_results.push_back(r);
        }
    }
    for (int enabled : {0, 1})
    {
        show_performance = enabled;
        for (int warm = 0; warm < 20; ++warm)
            for (int id = 0; id < 4; ++id)
                display_commit_frame(id, source.data, RK_FORMAT_BGR_888, 1280, 720, 1280, 720);
        std::vector<double> timings;
        const auto start = Clock::now();
        for (int frame = 0; frame < iterations; ++frame)
        {
            const auto begin = Clock::now();
            for (int id = 0; id < 4; ++id)
                display_commit_frame(id, source.data, RK_FORMAT_BGR_888, 1280, 720, 1280, 720);
            timings.push_back(std::chrono::duration<double, std::micro>(Clock::now() - begin).count() / 4);
        }
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        std::sort(timings.begin(), timings.end());
        double sum = 0;
        for (double t : timings)
            sum += t;
        std::cout << "{\"mode\":\"render\",\"performance_display\":" << enabled << ",\"frames\":" << iterations * 4
                  << ",\"mean_us_per_channel\":" << sum / timings.size()
                  << ",\"median_us_per_channel\":" << timings[timings.size() / 2]
                  << ",\"p95_us_per_channel\":" << timings[timings.size() * 95 / 100]
                  << ",\"fps_total\":" << iterations * 4 / elapsed << "}" << std::endl;
    }
    show_performance = 1;
    for (int id = 0; id < 4; ++id)
        g_pCtrl->channels_state[id].preview_rate.reset();
    for (int id = 0; id < 4; ++id)
        display_refresh_channel_overlays(id);
    const auto idle_start = Clock::now();
    for (int n = 0; n < 500; ++n)
        for (int id = 0; id < 4; ++id)
            display_refresh_channel_overlays(id);
    const double idle_us = std::chrono::duration<double, std::micro>(Clock::now() - idle_start).count() / 2000;
    std::cout << "{\"mode\":\"idle_unchanged\",\"mean_us_per_channel\":" << idle_us << "}" << std::endl;
    FrameRateCounter counter;
    uint64_t ticks = 0;
    const auto count_start = Clock::now();
    for (int n = 0; n < 1000000; ++n)
    {
        counter.tick(static_cast<uint64_t>(n) * 33);
        ticks += static_cast<uint64_t>(counter.value(static_cast<uint64_t>(n) * 33));
    }
    const double count_ns = std::chrono::duration<double, std::nano>(Clock::now() - count_start).count() / 1000000;
    std::cout << "{\"mode\":\"counter_tick_and_read\",\"mean_ns\":" << count_ns << ",\"checksum\":" << ticks << "}"
              << std::endl;
    for (int id = 0; id < 4; ++id)
        pthread_mutex_destroy(&g_pCtrl->chn_mtx[id]);
    delete g_pCtrl;
    g_pCtrl = nullptr;
}
