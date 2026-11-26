#pragma once
#include "application/runtime/app_ctrl.h"
#include "media_source.h"
#include "rtsp_dma.h"
#include <array>
#include <chrono>
#include <gst/rtsp-server/rtsp-server.h>
#include <map>
#include <mutex>
#include <pthread.h>
#include <string>

namespace rtsp_detail
{
struct ChannelPreviewFrame
{
    std::vector<unsigned char> bytes;
    int format = 0, width = 0, height = 0, stride_w = 0, stride_h = 0;
};

struct RtspStreamer
{
    int channel_id = -1;
    std::atomic<bool> retired{false};
    std::atomic<bool> preview_client_active{false};
    std::mutex frame_mutex, media_mutex;
    std::shared_ptr<const ChannelPreviewFrame> pending_frame;
    std::map<std::string, uint64_t> leases;
    GWeakRef media_ref;
    RtspStreamer()
    {
        g_weak_ref_init(&media_ref, nullptr);
    }
    ~RtspStreamer()
    {
        g_weak_ref_clear(&media_ref);
    }

    bool inited = false;

    /* 配置快照 (init 时读取一次; 这些字段不参与热重载) */
    int port = 8554;
    std::string path = "/live";
    int fps = 15;
    int bitrate = 4096; /* kbps；硬编在插件支持 bps 属性时同样应用 */
    bool use_hw = false;
    int width = 0; /* 拼接画布尺寸，或独立通道的原始视频尺寸 */
    int height = 0;
    int enc_w = 0; /* 编码画面尺寸；DMA 内存的对齐由 DmaFramePool 单独处理 */
    int enc_h = 0;

    /* GStreamer / GLib */
    GMainContext *ctx = nullptr;
    GMainLoop *loop = nullptr;
    GstRTSPServer *server = nullptr;
    guint attach_id = 0;

    /* 当前活跃的 appsrc (有客户端连接时由 media-configure 写入) */
    RtspMediaSource media_source;

    /* 线程句柄与标志 */
    pthread_t loop_tid = 0;
    pthread_t feeder_tid = 0;
    bool loop_running = false;
    bool feeder_running = false;
    std::atomic<bool> feeder_exit{false};

    /* 硬编只接收 DMA-BUF NV12；池初始化失败时 auto 模式回退软编。 */
    std::shared_ptr<DmaFramePool> dma_pool;
};

extern RtspStreamer g_st;
extern std::mutex g_channel_previews_mutex;
extern std::mutex g_preview_operations_mutex;
extern std::map<int, std::shared_ptr<RtspStreamer>> g_channel_previews;
extern std::array<std::atomic<int>, MAX_CHANNEL_NUM> g_channel_preview_active;
extern std::atomic<bool> g_preview_ready;
extern GSource *g_preview_expiry_source;
uint64_t preview_now_ms();
void stop_channel_preview(const std::shared_ptr<RtspStreamer> &state);
gboolean expire_channel_previews(gpointer);
void release_callback_state(gpointer data, GClosure *);
void on_media_configure(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data);
void *rtsp_feeder_thread(void *arg);
std::string build_launch_string(RtspStreamer &state);
} // namespace rtsp_detail
