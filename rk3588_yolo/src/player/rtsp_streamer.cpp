/**
 * @file rtsp_streamer.cpp
 * @brief 内置 RTSP 推流实现 (设计说明见 rtsp_streamer.h)
 *
 * 线程:
 *   rtsp_loop_thread   — 跑独立 GMainContext 上的 GMainLoop, 服务 gst-rtsp-server
 *   rtsp_feeder_thread — 以 rtsp_fps 周期读 g_disp 拼接大图, push 进 appsrc
 *
 * 数据流:
 *   display_worker[N] → commitImgtoDispBufMap → g_disp.front (RGB 拼接大图)
 *                                                     │ (本模块)
 *   rtsp_feeder_thread: display_lock 内 memcpy front → GstBuffer
 *                       → g_signal_emit_by_name(appsrc,"push-buffer")
 *                       → queue → videoconvert → (mpph264enc|x264enc)
 *                       → h264parse → rtph264pay(pay0) → gst-rtsp-server
 */
#include "rtsp_streamer.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <pthread.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include "../core/app_ctrl.h"
#include "display.h" /* display_lock / display_unlock */

/*======================== 模块状态 ========================*/
struct RtspStreamer
{
    bool inited = false;

    /* 配置快照 (init 时读取一次; 这些字段不参与热重载) */
    int         port    = 8554;
    std::string path    = "/live";
    int         fps     = 25;
    int         bitrate = 4096; /* kbps, 仅软件编码使用 */
    std::string codec   = "h264";
    std::string encoder = "auto"; /* "auto"/"hw"/"sw" */
    int         width   = 0; /* 源拼接大图尺寸 (= disp_width/disp_height) */
    int         height  = 0;
    int         enc_w   = 0; /* 送编码器/RTSP 的尺寸: 向上对齐到 16, 规避 MPP 编码器非对齐绿屏 */
    int         enc_h   = 0;

    /* GStreamer / GLib */
    GMainContext  *ctx       = nullptr;
    GMainLoop     *loop      = nullptr;
    GstRTSPServer *server    = nullptr;
    guint          attach_id = 0;

    /* 当前活跃的 appsrc (有客户端连接时由 media-configure 写入) */
    pthread_mutex_t appsrc_mtx = PTHREAD_MUTEX_INITIALIZER;
    GstElement     *appsrc     = nullptr;

    /* 线程句柄与标志 */
    pthread_t    loop_tid       = 0;
    pthread_t    feeder_tid     = 0;
    bool         loop_running   = false;
    bool         feeder_running = false;
    volatile int feeder_exit    = 0;
};

static RtspStreamer g_st;

/*======================== media 生命周期回调 ========================*/

/* 最后一个客户端断开 → 共享 media 反配置: 清掉 appsrc, feeder 随之空转。 */
static void on_media_unprepared(GstRTSPMedia *media, gpointer user_data)
{
    (void)media;
    (void)user_data;
    pthread_mutex_lock(&g_st.appsrc_mtx);
    if (g_st.appsrc)
    {
        gst_object_unref(g_st.appsrc);
        g_st.appsrc = nullptr;
    }
    pthread_mutex_unlock(&g_st.appsrc_mtx);
    printf("[RTSP] media unprepared (no clients)\n");
}

/* 客户端连接、media 创建时调用: 取出 appsrc, 设 caps/属性, 存给 feeder。 */
static void on_media_configure(GstRTSPMediaFactory *factory,
                               GstRTSPMedia *media, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GstElement *element = gst_rtsp_media_get_element(media);
    if (!element)
        return;
    GstElement *appsrc = gst_bin_get_by_name_recurse_up(GST_BIN(element), "mysrc");
    gst_object_unref(element);
    if (!appsrc)
    {
        fprintf(stderr, "[RTSP] media-configure: appsrc 'mysrc' not found\n");
        return;
    }

    /* 实时源 + 自动打时间戳 + 满了不阻塞(丢帧保实时) */
    g_object_set(G_OBJECT(appsrc),
                 "format", GST_FORMAT_TIME,
                 "is-live", TRUE,
                 "do-timestamp", TRUE,
                 "block", FALSE,
                 "max-bytes", (guint64)((gint64)g_st.enc_w * g_st.enc_h * 3 * 3),
                 nullptr);

    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "RGB",
        "width", G_TYPE_INT, g_st.enc_w,
        "height", G_TYPE_INT, g_st.enc_h,
        "framerate", GST_TYPE_FRACTION, g_st.fps, 1,
        nullptr);
    g_object_set(G_OBJECT(appsrc), "caps", caps, nullptr);
    gst_caps_unref(caps);

    pthread_mutex_lock(&g_st.appsrc_mtx);
    if (g_st.appsrc)
        gst_object_unref(g_st.appsrc);
    g_st.appsrc = appsrc; /* 持有 gst_bin_get_by_name_recurse_up 返回的引用 */
    pthread_mutex_unlock(&g_st.appsrc_mtx);

    g_signal_connect(media, "unprepared", G_CALLBACK(on_media_unprepared), nullptr);
    printf("[RTSP] client connected, media configured (%dx%d @%dfps)\n",
           g_st.enc_w, g_st.enc_h, g_st.fps);
}

/*======================== 推帧线程 ========================*/

static void *rtsp_feeder_thread(void *arg)
{
    (void)arg;
    const int    src_w       = g_st.width;
    const int    src_h       = g_st.height;
    const int    dst_w       = g_st.enc_w;
    const int    dst_h       = g_st.enc_h;
    const size_t src_stride  = (size_t)src_w * 3; /* RGB packed */
    const size_t dst_stride  = (size_t)dst_w * 3;
    const size_t frame_bytes = dst_stride * (size_t)dst_h;
    const bool   need_pad    = (dst_w != src_w) || (dst_h != src_h);
    const useconds_t period_us =
        (useconds_t)(1000000 / (g_st.fps > 0 ? g_st.fps : 25));

    while (g_pCtrl && g_pCtrl->isRunning && !g_st.feeder_exit)
    {
        GstElement *src = nullptr;
        pthread_mutex_lock(&g_st.appsrc_mtx);
        if (g_st.appsrc)
            src = (GstElement *)gst_object_ref(g_st.appsrc);
        pthread_mutex_unlock(&g_st.appsrc_mtx);

        if (src)
        {
            char *front = (g_pCtrl->pDispBuffer) ? *g_pCtrl->pDispBuffer : nullptr;
            if (front)
            {
                GstBuffer *buf = gst_buffer_new_allocate(nullptr, frame_bytes, nullptr);
                GstMapInfo map;
                if (buf && gst_buffer_map(buf, &map, GST_MAP_WRITE))
                {
                    /* 只在 display_lock 内读 front (与 commitImgtoDispBufMap 的 copyTo 互斥防撕裂)。
                     * 源是 src_w×src_h 拼接图; 编码尺寸 dst 已对齐到 16, 源贴左上, 右/下补黑边。*/
                    if (!need_pad)
                    {
                        display_lock();
                        memcpy(map.data, front, frame_bytes);
                        display_unlock();
                    }
                    else
                    {
                        display_lock();
                        for (int y = 0; y < src_h; ++y)
                            memcpy(map.data + (size_t)y * dst_stride,
                                   front + (size_t)y * src_stride, src_stride);
                        display_unlock();
                        /* 黑边填充 (buf 私有于本次迭代, 锁外做) */
                        if (dst_w > src_w)
                            for (int y = 0; y < src_h; ++y)
                                memset(map.data + (size_t)y * dst_stride + src_stride, 0,
                                       (size_t)(dst_w - src_w) * 3);
                        if (dst_h > src_h)
                            memset(map.data + (size_t)src_h * dst_stride, 0,
                                   (size_t)(dst_h - src_h) * dst_stride);
                    }
                    gst_buffer_unmap(buf, &map);

                    GstFlowReturn ret = GST_FLOW_OK;
                    /* 信号版 push-buffer 不夺取所有权(steal_ref=FALSE), 故下面仍需 unref。*/
                    g_signal_emit_by_name(src, "push-buffer", buf, &ret);
                    if (ret != GST_FLOW_OK)
                    {
                        static int warn_cnt = 0;
                        if ((warn_cnt++ % 200) == 0)
                            fprintf(stderr, "[RTSP] push-buffer ret=%d\n", (int)ret);
                    }
                }
                if (buf)
                    gst_buffer_unref(buf);
            }
            gst_object_unref(src);
        }
        usleep(period_us);
    }
    printf("[RTSP] feeder thread exit\n");
    return nullptr;
}

/*======================== 服务主循环线程 ========================*/

static void *rtsp_loop_thread(void *arg)
{
    (void)arg;
    g_main_context_push_thread_default(g_st.ctx);
    g_st.attach_id = gst_rtsp_server_attach(g_st.server, g_st.ctx);
    if (g_st.attach_id == 0)
        fprintf(stderr, "[RTSP] gst_rtsp_server_attach failed (port busy?)\n");
    else
        printf("[RTSP] server attached, entering service loop\n");
    g_main_loop_run(g_st.loop);
    g_main_context_pop_thread_default(g_st.ctx);
    printf("[RTSP] loop thread exit\n");
    return nullptr;
}

/*======================== 编码管线选择 ========================*/

/* 运行时探测硬件编码器: 有 mpph26xenc 用硬编, 否则回退 x26xenc 软编。
 * 注意 launch 字符串需用 ( ) 包裹, 且 payloader 必须命名为 pay0。*/
static std::string build_launch_string(void)
{
    const bool h265        = (g_st.codec == "h265" || g_st.codec == "hevc");
    const char *enc_hw     = h265 ? "mpph265enc" : "mpph264enc";
    const char *parse_elem = h265 ? "h265parse" : "h264parse";
    const char *pay_elem   = h265 ? "rtph265pay" : "rtph264pay";

    /* 选择硬编/软编:
     *   "sw"   强制软件 x26xenc
     *   "hw"   强制硬件 mpph26xenc (插件缺失时 pipeline 会报错, 便于暴露问题)
     *   "auto" 探测到 mpph26xenc 用硬编, 否则回退软编 (默认) */
    bool hw_available = false;
    GstElementFactory *hw_factory = gst_element_factory_find(enc_hw);
    if (hw_factory) { hw_available = true; gst_object_unref(hw_factory); }

    bool use_hw;
    if (g_st.encoder == "sw")      use_hw = false;
    else if (g_st.encoder == "hw") use_hw = true;
    else                           use_hw = hw_available; /* auto */

    char launch[1024];
    if (use_hw)
    {
        /* 硬件编码: 不显式设码率, 避免不同版本插件属性名差异导致 pipeline 解析失败 */
        snprintf(launch, sizeof(launch),
                 "( appsrc name=mysrc ! queue max-size-buffers=4 leaky=downstream "
                 "! videoconvert ! video/x-raw,format=NV12 ! %s "
                 "! %s ! %s name=pay0 pt=96 config-interval=1 )",
                 enc_hw, parse_elem, pay_elem);
        printf("[RTSP] encoder: HW %s (mode=%s, hw_available=%d)\n",
               enc_hw, g_st.encoder.c_str(), hw_available ? 1 : 0);
    }
    else
    {
        const char *enc_sw = h265 ? "x265enc" : "x264enc";
        snprintf(launch, sizeof(launch),
                 "( appsrc name=mysrc ! queue max-size-buffers=4 leaky=downstream "
                 "! videoconvert ! %s tune=zerolatency speed-preset=ultrafast bitrate=%d "
                 "! %s ! %s name=pay0 pt=96 config-interval=1 )",
                 enc_sw, g_st.bitrate, parse_elem, pay_elem);
        printf("[RTSP] encoder: SW %s (mode=%s, hw_available=%d, %dkbps)\n",
               enc_sw, g_st.encoder.c_str(), hw_available ? 1 : 0, g_st.bitrate);
    }
    return std::string(launch);
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
        enabled      = g_pCtrl->config.enable_rtsp;
        g_st.port    = g_pCtrl->config.rtsp_port > 0 ? g_pCtrl->config.rtsp_port : 8554;
        g_st.path    = g_pCtrl->config.rtsp_path.empty() ? "/live" : g_pCtrl->config.rtsp_path;
        g_st.fps     = g_pCtrl->config.rtsp_fps > 0 ? g_pCtrl->config.rtsp_fps : 25;
        g_st.bitrate = g_pCtrl->config.rtsp_bitrate > 0 ? g_pCtrl->config.rtsp_bitrate : 4096;
        g_st.codec   = g_pCtrl->config.rtsp_codec.empty() ? "h264" : g_pCtrl->config.rtsp_codec;
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

    g_st.width  = app_ctrl_get_disp_width();
    g_st.height = app_ctrl_get_disp_height();
    if (g_st.width <= 0 || g_st.height <= 0)
    {
        fprintf(stderr, "[RTSP] invalid disp size %dx%d, abort\n", g_st.width, g_st.height);
        return -1;
    }
    /* MPP 硬件编码器要求宽高对齐到 16 (1080 这类非 16 对齐高度会吐绿)。
     * 向上对齐, 源画面贴左上, 右/下补黑边。 */
    g_st.enc_w = (g_st.width + 15) & ~15;
    g_st.enc_h = (g_st.height + 15) & ~15;
    if (g_st.enc_w != g_st.width || g_st.enc_h != g_st.height)
        printf("[RTSP] encode size aligned to 16: %dx%d -> %dx%d (black-padded)\n",
               g_st.width, g_st.height, g_st.enc_w, g_st.enc_h);
    if (!g_pCtrl->pDispBuffer || !*g_pCtrl->pDispBuffer)
    {
        fprintf(stderr, "[RTSP] display buffer not allocated; ensure g_disp is allocated "
                        "when enable_rtsp=true (main.cpp step 5). abort\n");
        return -1;
    }

    /* ---- 独立 GMainContext + loop (与 GTK 主循环互不干扰) ---- */
    g_st.ctx  = g_main_context_new();
    g_st.loop = g_main_loop_new(g_st.ctx, FALSE);

    g_st.server = gst_rtsp_server_new();
    char service[16];
    snprintf(service, sizeof(service), "%d", g_st.port);
    g_object_set(g_st.server, "service", service, nullptr);

    GstRTSPMountPoints  *mounts  = gst_rtsp_server_get_mount_points(g_st.server);
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    const std::string launch = build_launch_string();
    gst_rtsp_media_factory_set_launch(factory, launch.c_str());
    gst_rtsp_media_factory_set_shared(factory, TRUE); /* 多客户端共享同一编码管线 */
    g_signal_connect(factory, "media-configure", G_CALLBACK(on_media_configure), nullptr);
    gst_rtsp_mount_points_add_factory(mounts, g_st.path.c_str(), factory);
    g_object_unref(mounts);

    g_st.inited      = true;
    g_st.feeder_exit = 0;

    if (pthread_create(&g_st.loop_tid, nullptr, rtsp_loop_thread, nullptr) != 0)
    {
        fprintf(stderr, "[RTSP] pthread_create loop_thread failed\n");
        g_st.inited = false;
        return -1;
    }
    g_st.loop_running = true;

    if (pthread_create(&g_st.feeder_tid, nullptr, rtsp_feeder_thread, nullptr) != 0)
        fprintf(stderr, "[RTSP] pthread_create feeder_thread failed (loop will be cleaned up at deinit)\n");
    else
        g_st.feeder_running = true;

    printf("[RTSP] streaming composited view at rtsp://<board-ip>:%d%s "
           "(codec=%s, %dfps, %dx%d)\n",
           g_st.port, g_st.path.c_str(), g_st.codec.c_str(),
           g_st.fps, g_st.width, g_st.height);
    return 0;
}

void rtsp_streamer_deinit(void)
{
    if (!g_st.inited)
        return;

    /* 先停 feeder, 再退服务循环 */
    g_st.feeder_exit = 1;
    if (g_st.feeder_running)
    {
        pthread_join(g_st.feeder_tid, nullptr);
        g_st.feeder_running = false;
    }

    if (g_st.loop)
        g_main_loop_quit(g_st.loop);
    if (g_st.loop_running)
    {
        pthread_join(g_st.loop_tid, nullptr);
        g_st.loop_running = false;
    }

    pthread_mutex_lock(&g_st.appsrc_mtx);
    if (g_st.appsrc)
    {
        gst_object_unref(g_st.appsrc);
        g_st.appsrc = nullptr;
    }
    pthread_mutex_unlock(&g_st.appsrc_mtx);

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

    g_st.inited = false;
    printf("[RTSP] streamer deinit done\n");
}
