#include "application/runtime/app_ctrl.h"
#include "application/runtime/pause_ctrl.h"
#include "media/display/display_render.h"

#include <cassert>
#include <iostream>

APP_CTRL *g_pCtrl = nullptr;
static std::shared_ptr<AppRuntimeSnapshot> runtime = std::make_shared<AppRuntimeSnapshot>();
static FrameRateCounter infer_rate;
static int show_performance = 1;

/* 使用真实渲染和缓冲写入路径，只替代应用配置与硬件推理查询。 */
int app_ctrl_get_performance_display()
{
    return show_performance;
}
int app_ctrl_has_channel(int id)
{
    return id == 0;
}
int app_ctrl_get_channel_display_order(int)
{
    return 0;
}
int app_ctrl_get_disp_width()
{
    return 640;
}
int app_ctrl_get_disp_height()
{
    return 160;
}
int app_ctrl_get_tile_cols()
{
    return 1;
}
int app_ctrl_get_tile_rows()
{
    return 1;
}
float app_ctrl_get_disp_fps(int)
{
    return g_pCtrl->channels_state[0].preview_rate.value();
}
std::shared_ptr<const AppRuntimeSnapshot> app_ctrl_get_runtime_snapshot()
{
    return runtime;
}
const ChannelConfig *app_ctrl_runtime_channel_config(const std::shared_ptr<const AppRuntimeSnapshot> &, int)
{
    return &runtime->config.channels[0];
}
const std::vector<RoiZone> *app_ctrl_runtime_channel_rois(const std::shared_ptr<const AppRuntimeSnapshot> &, int)
{
    return &runtime->roi_zones[0];
}
float inference_get_infer_fps(int)
{
    return infer_rate.value();
}

int main()
{
    g_pCtrl = new APP_CTRL();
    g_pCtrl->business_width = 640;
    g_pCtrl->business_height = 160;
    runtime->config.channels.emplace_back();
    pthread_mutex_init(&g_pCtrl->chn_mtx[0], nullptr);
    pause_ctrl::init(true);
    cv::Mat front(160, 640, CV_8UC3);
    char *front_data = reinterpret_cast<char *>(front.data);
    g_pCtrl->pDispBuffer = &front_data;
    auto &state = g_pCtrl->channels_state[0];
    const cv::Mat base(160, 640, CV_8UC3, cv::Scalar(11, 22, 33));
    // 当前显示缓存是 RGB，停帧后打开性能显示应按原有颜色创建底部背景。
    cv::cvtColor(base, state.tile_staging, cv::COLOR_BGR2RGB);
    state.tile_staging.copyTo(front);
    for (int i = 0; i < 25; ++i)
        state.preview_rate.tick();
    for (int i = 0; i < 10; ++i)
        infer_rate.tick();

    display_refresh_channel_overlays(0);
    const cv::Mat active = front.clone();
    assert(state.preview_rate.value() == 25); // 重绘叠加没有新增合成帧。
    for (int i = 0; i < 3; ++i)
        display_refresh_channel_overlays(0);
    assert(state.preview_rate.value() == 25);
    assert(cv::norm(active, front, cv::NORM_INF) == 0);

    pause_ctrl::toggle();
    display_refresh_channel_overlays(0);
    const cv::Mat paused = front.clone();
    assert(cv::norm(active, paused, cv::NORM_INF) > 0);
    pause_ctrl::toggle();
    state.preview_rate.reset();
    infer_rate.reset();
    // 模拟已过期的帧；刷新应显示与暂停时相同的两个零值。
    state.preview_rate.tick(performance_now_ms() - 2000);
    infer_rate.tick(performance_now_ms() - 2000);
    display_refresh_channel_overlays(0);
    assert(cv::norm(paused, front, cv::NORM_INF) == 0);
    assert(state.preview_rate.value() == 0);

    show_performance = 0;
    display_refresh_channel_overlays(0);
    cv::Mat expected;
    cv::cvtColor(base, expected, cv::COLOR_BGR2RGB);
    assert(cv::norm(front, expected, cv::NORM_INF) == 0); // 清除旧文字，无叠加残影。
    assert(state.performance_overlay.background.empty());
    assert(!state.performance_overlay.visible);
    assert(cv::norm(front.rowRange(0, 96), expected.rowRange(0, 96), cv::NORM_INF) == 0);

    // 模拟正常帧完成：关闭性能显示时不缓存任何背景；开启时只保存底部条带。
    vision::OverlayCache cache;
    vision::OverlayOptions params;
    params.inputW = 640;
    params.inputH = 160;
    cv::Mat bgr = base.clone();
    render_runtime_overlays(bgr, params, &cache);
    assert(cache.background.empty());
    show_performance = 1;
    render_runtime_overlays(bgr, params, &cache);
    const cv::Rect strip = vision::performance_overlay_bounds(bgr);
    assert(cache.background.size() == strip.size());
    assert(cv::norm(cache.background, base(strip), cv::NORM_INF) == 0);

    // 即使帧率和视频帧没变，RTT 更新/失效也必须刷新；USB/文件源隐藏 RTT。
    state.tile_staging = base.clone();
    cv::cvtColor(state.tile_staging, state.tile_staging, cv::COLOR_BGR2RGB);
    state.tile_staging.copyTo(front);
    state.performance_overlay = vision::OverlayCache{};
    runtime->config.channels[0].stream.src_type = "rtsp";
    state.network_rtt_ms = -1;
    display_refresh_channel_overlays(0);
    const cv::Mat unknown_rtt = front.clone();
    state.network_rtt_ms = 12.3f;
    display_refresh_channel_overlays(0);
    assert(cv::norm(front, unknown_rtt, cv::NORM_INF) > 0);
    state.network_rtt_ms = -1;
    display_refresh_channel_overlays(0);
    assert(cv::norm(front, unknown_rtt, cv::NORM_INF) == 0);
    runtime->config.channels[0].stream.src_type = "file";
    display_refresh_channel_overlays(0);
    assert(cv::norm(front, unknown_rtt, cv::NORM_INF) > 0);
    show_performance = 0;
    display_refresh_channel_overlays(0);
    assert(cv::norm(front, expected, cv::NORM_INF) == 0);
    show_performance = 1;

    // 窗口宽高翻倍时文字也应等比例变大；刷新背景必须覆盖所有文字，4K 下也不能留残影。
    cv::Rect previous_text;
    for (const cv::Size size : {cv::Size(480, 270), cv::Size(960, 540), cv::Size(1920, 1080), cv::Size(3840, 2160)})
    {
        const cv::Mat clean(size, CV_8UC3, cv::Scalar(11, 22, 33));
        cv::Mat rendered = clean.clone();
        vision::OverlayCache scaled_cache;
        vision::render_performance_overlay(rendered, params, &scaled_cache);
        cv::Mat difference, gray;
        cv::absdiff(rendered, clean, difference);
        cv::cvtColor(difference, gray, cv::COLOR_BGR2GRAY);
        const cv::Rect text = cv::boundingRect(gray);
        const cv::Rect background = vision::performance_overlay_bounds(rendered);
        assert(!text.empty() && (text & background) == text);
        assert(text.x > size.width / 2 && text.y > size.height / 2);
        if (!previous_text.empty())
        {
            assert(text.width > previous_text.width * 1.7 && text.width < previous_text.width * 2.3);
            assert(text.height > previous_text.height * 1.5 && text.height < previous_text.height * 2.5);
        }
        scaled_cache.background.copyTo(rendered(background));
        assert(cv::norm(rendered, clean, cv::NORM_INF) == 0);
        previous_text = text;
    }

    // 固定字号下，窗口变化不改变文字尺寸和右下边距，缓存仍能完整恢复底图。
    for (int font_height : {18, 36})
    {
        vision::OverlayOptions fixed = params;
        fixed.fps_font_height = font_height;
        cv::Size text_size;
        cv::Point bottom_right_gap;
        for (const cv::Size size : {cv::Size(960, 540), cv::Size(1920, 1080)})
        {
            const cv::Mat clean(size, CV_8UC3, cv::Scalar(11, 22, 33));
            cv::Mat rendered = clean.clone();
            vision::OverlayCache fixed_cache;
            render_runtime_overlays(rendered, fixed, &fixed_cache);
            cv::Mat difference, gray;
            cv::absdiff(rendered, clean, difference);
            cv::cvtColor(difference, gray, cv::COLOR_BGR2GRAY);
            const cv::Rect text = cv::boundingRect(gray);
            const cv::Rect background = vision::performance_overlay_bounds(rendered, fixed);
            assert(!text.empty() && (text & background) == text);
            const cv::Point gap(size.width - text.br().x, size.height - text.br().y);
            if (!text_size.empty())
            {
                assert(text.size() == text_size);
                assert(gap == bottom_right_gap);
            }
            text_size = text.size();
            bottom_right_gap = gap;
            fixed_cache.background.copyTo(rendered(background));
            assert(cv::norm(rendered, clean, cv::NORM_INF) == 0);
        }
    }

    // swap_rb 路径的缓存仍为 BGR，停帧刷新不能额外交换红蓝。
    runtime->config.channels[0].swap_rb = true;
    state.tile_staging = base.clone();
    state.tile_staging.copyTo(front);
    state.performance_overlay = vision::OverlayCache{};
    display_refresh_channel_overlays(0);
    show_performance = 0;
    display_refresh_channel_overlays(0);
    assert(cv::norm(front, base, cv::NORM_INF) == 0);
    pthread_mutex_destroy(&g_pCtrl->chn_mtx[0]);
    delete g_pCtrl;
    g_pCtrl = nullptr;
    std::cout << "Idle/paused overlay refresh and frame counting regressions passed\n";
}
