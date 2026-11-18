/**
 * @file rtsp_streamer.cpp
 * @brief 内置 RTSP 推流实现 (设计说明见 rtsp_streamer.h)
 *
 * 线程:
 *   rtsp_loop_thread   — 跑独立 GMainContext 上的 GMainLoop, 服务 gst-rtsp-server
 *   rtsp_feeder_thread — 以引擎预览安全上限读 g_disp 拼接大图, push 进 appsrc
 *
 * 数据流:
 *   display_worker[N] → display_commit_frame → g_disp.front (RGB 拼接大图)
 *                                                     │ (本模块)
 *   rtsp_feeder_thread: display_lock 内取得一致画面
 *                       → 硬编时复制到 RGB DMA-BUF，再由 RGA 写入 NV12 DMA-BUF；软编时保持 RGB
 *                       → g_signal_emit_by_name(appsrc,"push-buffer")
 *                       → queue → (mpph264enc|videoconvert→x264enc)
 *                       → h264parse → rtph264pay(pay0) → gst-rtsp-server
 */
#include "rtsp_streamer.h"
#include "media/display/display.h"
#include "media/display/display_render.h"
#include "rtsp_streamer_internal.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

namespace rtsp_detail
{
uint64_t preview_now_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
RtspStreamer g_st;
std::mutex g_channel_previews_mutex;
// Serialize lifecycle operations without blocking per-frame map readers.
std::mutex g_preview_operations_mutex;
std::map<int, std::shared_ptr<RtspStreamer>> g_channel_previews;
// Decode callbacks never wait on the control lock to query client activity.
std::array<std::atomic<int>, MAX_CHANNEL_NUM> g_channel_preview_active{};
std::atomic<bool> g_preview_ready{false};
GSource *g_preview_expiry_source = nullptr;
} // namespace rtsp_detail

using namespace rtsp_detail;

static void *rtsp_loop_thread(void *arg)
{
    (void)arg;
    g_main_context_push_thread_default(g_st.ctx);
    printf("[RTSP] server attached, entering service loop\n");
    g_main_loop_run(g_st.loop);
    g_main_context_pop_thread_default(g_st.ctx);
    printf("[RTSP] loop thread exit\n");
    return nullptr;
}

/*======================== 生命周期 ========================*/

int rtsp_streamer_init(void)
{
    if (!g_pCtrl)
        return -1;
    if (g_st.inited)
        return 0;

    /* ---- 读取配置快照 ---- */
    bool enabled = false;
    {
        pthread_rwlock_rdlock(&g_pCtrl->mtx);
        enabled = g_pCtrl->config.enable_rtsp;
        g_st.port = g_pCtrl->config.rtsp_port > 0 ? g_pCtrl->config.rtsp_port : 8554;
        g_st.path = g_pCtrl->config.rtsp_path.empty() ? "/live" : g_pCtrl->config.rtsp_path;
        g_st.fps = constants::PREVIEW_MAX_FPS;
        g_st.bitrate = g_pCtrl->config.rtsp_bitrate > 0 ? g_pCtrl->config.rtsp_bitrate : 4096;
        g_st.codec = g_pCtrl->config.rtsp_codec.empty() ? "h264" : g_pCtrl->config.rtsp_codec;
        g_st.encoder = g_pCtrl->config.rtsp_encoder.empty() ? "auto" : g_pCtrl->config.rtsp_encoder;
        pthread_rwlock_unlock(&g_pCtrl->mtx);
    }
    if (!enabled)
    {
        printf("[RTSP] disabled (enable_rtsp=false)\n");
        return 0;
    }

    /* mount point 必须以 '/' 开头 */
    if (g_st.path.empty() || g_st.path[0] != '/')
        g_st.path = std::string("/") + g_st.path;

    g_st.width = app_ctrl_get_disp_width();
    g_st.height = app_ctrl_get_disp_height();
    if (g_st.width <= 0 || g_st.height <= 0)
    {
        fprintf(stderr, "[RTSP] invalid disp size %dx%d, abort\n", g_st.width, g_st.height);
        return -1;
    }
    /* 拼接流沿用已有的对齐画布尺寸；独立通道直接使用源视频尺寸。
     * DMA 存储的行宽与可见画面尺寸分别处理。 */
    g_st.enc_w = (g_st.width + 15) & ~15;
    g_st.enc_h = (g_st.height + 15) & ~15;
    if (g_st.enc_w != g_st.width || g_st.enc_h != g_st.height)
        printf("[RTSP] encode size aligned to 16: %dx%d -> %dx%d (black-padded)\n", g_st.width, g_st.height, g_st.enc_w,
               g_st.enc_h);
    if (!g_pCtrl->pDispBuffer || !*g_pCtrl->pDispBuffer)
    {
        fprintf(stderr, "[RTSP] display buffer not allocated; ensure g_disp is allocated "
                        "when enable_rtsp=true (main.cpp step 5). abort\n");
        return -1;
    }

    /* ---- 独立 GMainContext + loop (与 GTK 主循环互不干扰) ---- */
    g_st.ctx = g_main_context_new();
    g_st.loop = g_main_loop_new(g_st.ctx, FALSE);

    g_st.server = gst_rtsp_server_new();
    char service[16];
    snprintf(service, sizeof(service), "%d", g_st.port);
    g_object_set(g_st.server, "service", service, nullptr);

    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(g_st.server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    const std::string launch = build_launch_string(g_st);
    gst_rtsp_media_factory_set_launch(factory, launch.c_str());
    gst_rtsp_media_factory_set_shared(factory, TRUE); /* 多客户端共享同一编码管线 */
    g_signal_connect(factory, "media-configure", G_CALLBACK(on_media_configure), nullptr);
    gst_rtsp_mount_points_add_factory(mounts, g_st.path.c_str(), factory);
    g_object_unref(mounts);

    /* 端口绑定必须在 init 返回前同步完成。
     * 旧实现在线程里异步 attach，init 会在结果未知时继续启动 feeder，
     * 甚至输出“streaming”成功日志。端口被旧进程占用时，Web 随后会连到
     * 旧 RTSP 服务，表现为通道数量和布局都与当前配置不一致。 */
    g_st.attach_id = gst_rtsp_server_attach(g_st.server, g_st.ctx);
    if (g_st.attach_id == 0)
    {
        fprintf(stderr,
                "[RTSP] FATAL: cannot bind rtsp://0.0.0.0:%d%s "
                "(port already in use?)\n",
                g_st.port, g_st.path.c_str());
        g_object_unref(g_st.server);
        g_st.server = nullptr;
        g_main_loop_unref(g_st.loop);
        g_st.loop = nullptr;
        g_main_context_unref(g_st.ctx);
        g_st.ctx = nullptr;
        g_st.dma_pool.reset();
        return -1;
    }

    g_st.inited = true;
    g_st.retired.store(false);
    g_st.feeder_exit.store(false);

    if (pthread_create(&g_st.loop_tid, nullptr, rtsp_loop_thread, nullptr) != 0)
    {
        fprintf(stderr, "[RTSP] pthread_create loop_thread failed\n");
        rtsp_streamer_deinit();
        return -1;
    }
    g_st.loop_running = true;

    if (pthread_create(&g_st.feeder_tid, nullptr, rtsp_feeder_thread, nullptr) != 0)
    {
        fprintf(stderr, "[RTSP] pthread_create feeder_thread failed\n");
        rtsp_streamer_deinit();
        return -1;
    }
    else
        g_st.feeder_running = true;

    {
        std::lock_guard<std::mutex> lock(g_channel_previews_mutex);
        g_preview_expiry_source = g_timeout_source_new(1000);
        g_source_set_callback(g_preview_expiry_source, expire_channel_previews, nullptr, nullptr);
        g_source_attach(g_preview_expiry_source, g_st.ctx);
        g_preview_ready.store(true);
    }
    printf("[RTSP] streaming composited view at rtsp://<board-ip>:%d%s "
           "(codec=%s, %dfps, %dx%d)\n",
           g_st.port, g_st.path.c_str(), g_st.codec.c_str(), g_st.fps, g_st.width, g_st.height);
    return 0;
}

int rtsp_streamer_has_active_client(void)
{
    return g_st.media_source.active() ? 1 : 0;
}

void rtsp_streamer_deinit(void)
{
    if (!g_st.inited)
        return;

    {
        std::lock_guard<std::mutex> operation(g_preview_operations_mutex);
        g_preview_ready.store(false);
        if (g_preview_expiry_source)
        {
            g_source_destroy(g_preview_expiry_source);
            g_source_unref(g_preview_expiry_source);
            g_preview_expiry_source = nullptr;
        }
        std::vector<std::shared_ptr<RtspStreamer>> retired;
        {
            std::lock_guard<std::mutex> lock(g_channel_previews_mutex);
            for (const auto &entry : g_channel_previews)
                retired.push_back(entry.second);
            g_channel_previews.clear();
        }
        for (const auto &state : retired)
            stop_channel_preview(state);
    }
    /* 先停 feeder, 再退服务循环 */
    GstRTSPMedia *composite_media = nullptr;
    {
        std::lock_guard<std::mutex> lifecycle(g_st.media_mutex);
        g_st.retired.store(true);
        g_st.feeder_exit.store(true);
        composite_media = GST_RTSP_MEDIA(g_weak_ref_get(&g_st.media_ref));
        g_st.media_source.clear();
    }
    if (g_st.feeder_running)
    {
        pthread_join(g_st.feeder_tid, nullptr);
        g_st.feeder_running = false;
    }

    if (composite_media)
    {
        gst_rtsp_media_unprepare(composite_media);
        g_object_unref(composite_media);
    }
    // Queue the quit on the context: quit-before-run is otherwise lost when
    // a startup failure races the new loop thread.
    if (g_st.loop && g_st.loop_running)
    {
        GSource *quit = g_idle_source_new();
        g_source_set_callback(
            quit,
            [](gpointer loop) -> gboolean {
                g_main_loop_quit(static_cast<GMainLoop *>(loop));
                return G_SOURCE_REMOVE;
            },
            g_st.loop, nullptr);
        g_source_attach(quit, g_st.ctx);
        g_source_unref(quit);
    }
    if (g_st.loop_running)
    {
        pthread_join(g_st.loop_tid, nullptr);
        g_st.loop_running = false;
    }

    g_st.media_source.clear();

    if (g_st.server)
    {
        g_object_unref(g_st.server); /* 连带释放 mount points / factory */
        g_st.server = nullptr;
    }
    if (g_st.loop)
    {
        g_main_loop_unref(g_st.loop);
        g_st.loop = nullptr;
    }
    if (g_st.ctx)
    {
        g_main_context_unref(g_st.ctx);
        g_st.ctx = nullptr;
    }
    g_st.dma_pool.reset();

    g_st.inited = false;
    printf("[RTSP] streamer deinit done\n");
}
