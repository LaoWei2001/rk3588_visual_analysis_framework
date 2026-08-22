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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/video/video.h>

#include "runtime/app_ctrl.h"
#include "pipeline/frame_transform.h"
#include "display/display.h" /* display_lock / display_unlock */

namespace
{
static constexpr size_t RTSP_DMA_OUTPUT_SLOTS = 6;

struct DmaFrameSlot
{
    int fd{-1};
    void *mapped{MAP_FAILED};
    size_t size{0};
    std::shared_ptr<RgaImportedBuffer> rga;
    std::atomic<bool> in_use{false};

    ~DmaFrameSlot()
    {
        rga.reset();
        if (mapped != MAP_FAILED)
            munmap(mapped, size);
        if (fd >= 0)
            close(fd);
    }
};

static bool dma_buf_sync_cpu(int fd, bool start)
{
    struct dma_buf_sync sync_request{};
    sync_request.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_WRITE;
    int ret;
    do
    {
        ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync_request);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0 && errno != ENOTTY)
    {
        fprintf(stderr, "[RTSP] DMA_BUF_IOCTL_SYNC failed: fd=%d errno=%d (%s)\n", fd, errno, strerror(errno));
        return false;
    }
    return true;
}

static int open_rtsp_dma_heap()
{
    static const char *const heap_paths[] = {
        "/dev/dma_heap/rk-dma-heap-cma", "/dev/dma_heap/linux,cma", "/dev/dma_heap/cma",
        "/dev/dma_heap/system-uncached", "/dev/dma_heap/system"};
    for (const char *path : heap_paths)
    {
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0)
        {
            printf("[RTSP] DMA heap selected: %s\n", path);
            return fd;
        }
    }
    return -1;
}

static std::shared_ptr<DmaFrameSlot> allocate_dma_slot(int heap_fd, size_t size, int width, int height,
                                                       int stride_w, int stride_h, int format)
{
    struct dma_heap_allocation_data allocation{};
    allocation.len = size;
    allocation.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation) < 0)
    {
        fprintf(stderr, "[RTSP] DMA_HEAP_IOCTL_ALLOC failed: size=%zu errno=%d (%s)\n", size, errno,
                strerror(errno));
        return nullptr;
    }

    const int dma_fd = static_cast<int>(allocation.fd);
    void *mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, dma_fd, 0);
    if (mapped == MAP_FAILED)
    {
        fprintf(stderr, "[RTSP] mmap DMA-BUF failed: size=%zu errno=%d (%s)\n", size, errno, strerror(errno));
        close(dma_fd);
        return nullptr;
    }

    auto imported = rga_import_src_fd(dma_fd, width, height, stride_w, stride_h, format);
    if (!imported)
    {
        fprintf(stderr, "[RTSP] RGA cannot import allocated DMA-BUF: fd=%d format=%d\n", dma_fd, format);
        munmap(mapped, size);
        close(dma_fd);
        return nullptr;
    }

    auto slot = std::make_shared<DmaFrameSlot>();
    slot->fd = dma_fd;
    slot->mapped = mapped;
    slot->size = size;
    slot->rga = std::move(imported);
    return slot;
}

struct DmaFramePool
{
    std::shared_ptr<DmaFrameSlot> rgb_source;
    std::vector<std::shared_ptr<DmaFrameSlot>> nv12_outputs;
    int src_w{0};
    int src_h{0};
    int out_stride_w{0};
    int out_stride_h{0};

    static std::shared_ptr<DmaFramePool> create(int source_width, int source_height, int output_width,
                                                int output_height)
    {
        const int heap_fd = open_rtsp_dma_heap();
        if (heap_fd < 0)
        {
            fprintf(stderr, "[RTSP] no usable DMA heap found; hardware encoder DMA path unavailable\n");
            return nullptr;
        }

        auto pool = std::make_shared<DmaFramePool>();
        pool->src_w = source_width;
        pool->src_h = source_height;
        pool->out_stride_w = output_width;
        pool->out_stride_h = output_height;
        pool->rgb_source = allocate_dma_slot(heap_fd, static_cast<size_t>(source_width) * source_height * 3,
                                             source_width, source_height, source_width, source_height,
                                             RK_FORMAT_RGB_888);
        const size_t nv12_size = static_cast<size_t>(output_width) * output_height * 3 / 2;
        for (size_t index = 0; pool->rgb_source && index < RTSP_DMA_OUTPUT_SLOTS; ++index)
        {
            auto output = allocate_dma_slot(heap_fd, nv12_size, output_width, output_height, output_width,
                                            output_height, RK_FORMAT_YCbCr_420_SP);
            if (!output)
                break;
            if (!dma_buf_sync_cpu(output->fd, true))
                break;
            memset(output->mapped, 0, static_cast<size_t>(output_width) * output_height);
            memset(static_cast<unsigned char *>(output->mapped) + static_cast<size_t>(output_width) * output_height,
                   128, nv12_size - static_cast<size_t>(output_width) * output_height);
            if (!dma_buf_sync_cpu(output->fd, false))
                break;
            pool->nv12_outputs.push_back(std::move(output));
        }
        close(heap_fd);

        if (!pool->rgb_source || pool->nv12_outputs.size() != RTSP_DMA_OUTPUT_SLOTS)
        {
            fprintf(stderr, "[RTSP] DMA frame pool initialization failed (%zu/%zu output slots)\n",
                    pool->nv12_outputs.size(), RTSP_DMA_OUTPUT_SLOTS);
            return nullptr;
        }
        printf("[RTSP] DMA frame pool ready: RGB source + %zu NV12 output slots\n", pool->nv12_outputs.size());
        return pool;
    }

    std::shared_ptr<DmaFrameSlot> acquire_output()
    {
        for (const auto &slot : nv12_outputs)
        {
            bool expected = false;
            if (slot->in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return slot;
        }
        return nullptr;
    }

    void release_output(const std::shared_ptr<DmaFrameSlot> &slot)
    {
        if (slot)
            slot->in_use.store(false, std::memory_order_release);
    }

    bool begin_rgb_write()
    {
        return rgb_source && dma_buf_sync_cpu(rgb_source->fd, true);
    }

    bool end_rgb_write()
    {
        return rgb_source && dma_buf_sync_cpu(rgb_source->fd, false);
    }

    bool convert_to_nv12(const std::shared_ptr<DmaFrameSlot> &output)
    {
        if (!rgb_source || !rgb_source->rga || !output || !output->rga)
            return false;
        return rga_convert_resize_handle(-1, *rgb_source->rga, output->fd, src_w, src_h, out_stride_w, out_stride_h,
                                         RK_FORMAT_YCbCr_420_SP, static_cast<int>(output->rga->handle));
    }
};

struct DmaSlotLease
{
    std::shared_ptr<DmaFrameSlot> slot;
};

static void release_dma_slot_when_memory_dies(gpointer user_data, GstMiniObject *object)
{
    (void)object;
    std::unique_ptr<DmaSlotLease> lease(static_cast<DmaSlotLease *>(user_data));
    lease->slot->in_use.store(false, std::memory_order_release);
}

static GstBuffer *make_dmabuf_video_buffer(GstAllocator *allocator, const std::shared_ptr<DmaFrameSlot> &slot,
                                           int width, int height)
{
    if (!allocator || !slot)
        return nullptr;
    const int exported_fd = fcntl(slot->fd, F_DUPFD_CLOEXEC, 0);
    if (exported_fd < 0)
        return nullptr;

    GstMemory *memory = gst_dmabuf_allocator_alloc(allocator, exported_fd, slot->size);
    if (!memory)
    {
        close(exported_fd);
        return nullptr;
    }

    GstBuffer *buffer = gst_buffer_new();
    auto *lease = new DmaSlotLease{slot};
    gst_mini_object_weak_ref(GST_MINI_OBJECT(memory), release_dma_slot_when_memory_dies, lease);
    if (!buffer)
    {
        gst_memory_unref(memory);
        return nullptr;
    }
    gst_buffer_append_memory(buffer, memory);

    gsize offsets[GST_VIDEO_MAX_PLANES] = {0};
    gint strides[GST_VIDEO_MAX_PLANES] = {0};
    offsets[1] = static_cast<gsize>(width) * height;
    strides[0] = width;
    strides[1] = width;
    gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12, width, height, 2,
                                   offsets, strides);

    return buffer;
}
} // namespace

/*======================== 模块状态 ========================*/
struct RtspStreamer
{
    bool inited = false;

    /* 配置快照 (init 时读取一次; 这些字段不参与热重载) */
    int port = 8554;
    std::string path = "/live";
    int fps = 15;
    int bitrate = 4096; /* kbps；硬编在插件支持 bps 属性时同样应用 */
    std::string codec = "h264";
    std::string encoder = "auto"; /* "auto"/"hw"/"sw" */
    bool use_hw = false;
    int width = 0;                /* 源拼接大图尺寸 (= disp_width/disp_height) */
    int height = 0;
    int enc_w = 0; /* 送编码器/RTSP 的尺寸: 向上对齐到 16, 规避 MPP 编码器非对齐绿屏 */
    int enc_h = 0;

    /* GStreamer / GLib */
    GMainContext *ctx = nullptr;
    GMainLoop *loop = nullptr;
    GstRTSPServer *server = nullptr;
    guint attach_id = 0;

    /* 当前活跃的 appsrc (有客户端连接时由 media-configure 写入) */
    pthread_mutex_t appsrc_mtx = PTHREAD_MUTEX_INITIALIZER;
    GstElement *appsrc = nullptr;

    /* 线程句柄与标志 */
    pthread_t loop_tid = 0;
    pthread_t feeder_tid = 0;
    bool loop_running = false;
    bool feeder_running = false;
    std::atomic<bool> feeder_exit{false};
    std::atomic<bool> client_active{false};

    /* 硬编只接收 DMA-BUF NV12；池初始化失败时 auto 模式回退软编。 */
    std::shared_ptr<DmaFramePool> dma_pool;
};

static RtspStreamer g_st;

/*======================== media 生命周期回调 ========================*/

static void set_encoder_int_if_supported(GstElement *encoder, const char *property, int value)
{
    if (!encoder)
        return;
    GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), property);
    if (!spec)
        return;

    GValue typed_value = G_VALUE_INIT;
    g_value_init(&typed_value, G_PARAM_SPEC_VALUE_TYPE(spec));
    if (G_VALUE_HOLDS_INT(&typed_value))
        g_value_set_int(&typed_value, value);
    else if (G_VALUE_HOLDS_UINT(&typed_value))
        g_value_set_uint(&typed_value, static_cast<guint>(std::max(0, value)));
    else if (G_VALUE_HOLDS_INT64(&typed_value))
        g_value_set_int64(&typed_value, value);
    else if (G_VALUE_HOLDS_UINT64(&typed_value))
        g_value_set_uint64(&typed_value, static_cast<guint64>(std::max(0, value)));
    else
    {
        g_value_unset(&typed_value);
        return;
    }
    g_object_set_property(G_OBJECT(encoder), property, &typed_value);
    g_value_unset(&typed_value);
}

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
    g_st.client_active.store(false, std::memory_order_release);
    printf("[RTSP] media unprepared (no clients)\n");
}

/* 客户端连接、media 创建时调用: 取出 appsrc, 设 caps/属性, 存给 feeder。 */
static void on_media_configure(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data)
{
    (void)factory;
    (void)user_data;

    GstElement *element = gst_rtsp_media_get_element(media);
    if (!element)
        return;
    GstElement *appsrc = gst_bin_get_by_name_recurse_up(GST_BIN(element), "mysrc");
    GstElement *encoder = gst_bin_get_by_name_recurse_up(GST_BIN(element), "video_encoder");
    gst_object_unref(element);
    if (!appsrc)
    {
        if (encoder)
            gst_object_unref(encoder);
        fprintf(stderr, "[RTSP] media-configure: appsrc 'mysrc' not found\n");
        return;
    }

    /* 一秒一个关键帧：fMP4/MSE 客户端连接和重连无需等待过长 GOP。
     * MPP 插件版本间属性并不完全一致，因此仅设置实际存在的属性。 */
    if (encoder)
    {
        set_encoder_int_if_supported(encoder, "gop", std::max(1, g_st.fps));
        set_encoder_int_if_supported(encoder, "key-int-max", std::max(1, g_st.fps));
        set_encoder_int_if_supported(encoder, "bps", std::max(1, g_st.bitrate) * 1000);
        gst_object_unref(encoder);
    }

    /* 实时源 + 自动打时间戳 + 满了不阻塞(丢帧保实时) */
    const guint64 frame_bytes = g_st.use_hw ? (guint64)g_st.enc_w * g_st.enc_h * 3 / 2
                                            : (guint64)g_st.enc_w * g_st.enc_h * 3;
    g_object_set(G_OBJECT(appsrc), "format", GST_FORMAT_TIME, "is-live", TRUE, "do-timestamp", TRUE, "block", FALSE,
                 "max-bytes", frame_bytes * 3, nullptr);

    GstCaps *caps =
        gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, g_st.use_hw ? "NV12" : "RGB", "width",
                            G_TYPE_INT, g_st.enc_w, "height", G_TYPE_INT, g_st.enc_h, "framerate", GST_TYPE_FRACTION,
                            g_st.fps, 1, nullptr);
    g_object_set(G_OBJECT(appsrc), "caps", caps, nullptr);
    gst_caps_unref(caps);

    pthread_mutex_lock(&g_st.appsrc_mtx);
    if (g_st.appsrc)
        gst_object_unref(g_st.appsrc);
    g_st.appsrc = appsrc; /* 持有 gst_bin_get_by_name_recurse_up 返回的引用 */
    pthread_mutex_unlock(&g_st.appsrc_mtx);
    g_st.client_active.store(true, std::memory_order_release);

    g_signal_connect(media, "unprepared", G_CALLBACK(on_media_unprepared), nullptr);
    printf("[RTSP] client connected, media configured (%dx%d @%dfps)\n", g_st.enc_w, g_st.enc_h, g_st.fps);
}

/*======================== 推帧线程 ========================*/

static void *rtsp_feeder_thread(void *arg)
{
    (void)arg;
    const int src_w = g_st.width;
    const int src_h = g_st.height;
    const int dst_w = g_st.enc_w;
    const int dst_h = g_st.enc_h;
    const size_t src_stride = (size_t)src_w * 3; /* RGB packed */
    const size_t dst_stride = (size_t)dst_w * 3;
    const size_t rgb_frame_bytes = dst_stride * (size_t)dst_h;
    const bool need_pad = (dst_w != src_w) || (dst_h != src_h);
    const auto period = std::chrono::microseconds(1000000 / std::max(1, g_st.fps));
    auto next_wakeup = std::chrono::steady_clock::now();
    const std::shared_ptr<DmaFramePool> dma_pool = g_st.dma_pool;
    GstAllocator *dmabuf_allocator = g_st.use_hw ? gst_dmabuf_allocator_new() : nullptr;

    auto push_buffer = [](GstElement *appsrc, GstBuffer *buffer) {
        GstFlowReturn ret = GST_FLOW_OK;
        g_signal_emit_by_name(appsrc, "push-buffer", buffer, &ret);
        if (ret != GST_FLOW_OK)
        {
            static int warn_cnt = 0;
            if ((warn_cnt++ % 200) == 0)
                fprintf(stderr, "[RTSP] push-buffer ret=%d\n", (int)ret);
        }
    };

    while (g_pCtrl && g_pCtrl->isRunning.load() && !g_st.feeder_exit.load())
    {
        GstElement *src = nullptr;
        pthread_mutex_lock(&g_st.appsrc_mtx);
        if (g_st.appsrc)
            src = (GstElement *)gst_object_ref(g_st.appsrc);
        pthread_mutex_unlock(&g_st.appsrc_mtx);

        if (src)
        {
            char *front = (g_pCtrl->pDispBuffer) ? *g_pCtrl->pDispBuffer : nullptr;
            if (front && g_st.use_hw && dma_pool && dmabuf_allocator)
            {
                auto output = dma_pool->acquire_output();
                bool frame_ready = output && dma_pool->begin_rgb_write();
                if (frame_ready)
                {
                    if (display_try_lock())
                    {
                        memcpy(dma_pool->rgb_source->mapped, front, static_cast<size_t>(src_w) * src_h * 3);
                        display_unlock();
                    }
                    else
                        frame_ready = false;
                    if (!dma_pool->end_rgb_write())
                        frame_ready = false;
                }
                if (frame_ready)
                    frame_ready = dma_pool->convert_to_nv12(output);

                GstBuffer *buffer =
                    frame_ready ? make_dmabuf_video_buffer(dmabuf_allocator, output, dst_w, dst_h) : nullptr;
                if (buffer)
                {
                    push_buffer(src, buffer);
                    gst_buffer_unref(buffer);
                }
                else
                    dma_pool->release_output(output);
            }
            else if (front && !g_st.use_hw)
            {
                GstBuffer *buffer = gst_buffer_new_allocate(nullptr, rgb_frame_bytes, nullptr);
                GstMapInfo map;
                bool frame_ready = buffer && gst_buffer_map(buffer, &map, GST_MAP_WRITE);
                if (frame_ready)
                {
                    if (display_try_lock())
                    {
                        if (!need_pad)
                            memcpy(map.data, front, rgb_frame_bytes);
                        else
                            for (int y = 0; y < src_h; ++y)
                                memcpy(map.data + (size_t)y * dst_stride, front + (size_t)y * src_stride, src_stride);
                        display_unlock();
                    }
                    else
                        frame_ready = false;

                    if (frame_ready && need_pad)
                    {
                        if (dst_w > src_w)
                            for (int y = 0; y < src_h; ++y)
                                memset(map.data + (size_t)y * dst_stride + src_stride, 0,
                                       (size_t)(dst_w - src_w) * 3);
                        if (dst_h > src_h)
                            memset(map.data + (size_t)src_h * dst_stride, 0,
                                   (size_t)(dst_h - src_h) * dst_stride);
                    }
                    gst_buffer_unmap(buffer, &map);
                }
                if (frame_ready)
                    push_buffer(src, buffer);
                if (buffer)
                    gst_buffer_unref(buffer);
            }
            gst_object_unref(src);
        }
        /* 绝对节拍：RGA/编码前准备耗时包含在本周期内，不再“处理耗时 + 固定 sleep”
         * 造成目标30FPS实际只有二十几帧。严重超时时直接从当前时刻重新起算，禁止追帧突发。 */
        next_wakeup += period;
        const auto now = std::chrono::steady_clock::now();
        if (next_wakeup + period < now)
            next_wakeup = now;
        std::this_thread::sleep_until(next_wakeup);
    }
    if (dmabuf_allocator)
        gst_object_unref(dmabuf_allocator);
    printf("[RTSP] feeder thread exit\n");
    return nullptr;
}

/*======================== 服务主循环线程 ========================*/

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

/*======================== 编码管线选择 ========================*/

/* 运行时探测硬件编码器: 有 mpph26xenc 用硬编, 否则回退 x26xenc 软编。
 * 注意 launch 字符串需用 ( ) 包裹, 且 payloader 必须命名为 pay0。*/
static std::string build_launch_string(void)
{
    const bool h265 = (g_st.codec == "h265" || g_st.codec == "hevc");
    const char *enc_hw = h265 ? "mpph265enc" : "mpph264enc";
    const char *parse_elem = h265 ? "h265parse" : "h264parse";
    const char *pay_elem = h265 ? "rtph265pay" : "rtph264pay";

    /* 选择硬编/软编:
     *   "hw"   强制硬件 mpph26xenc (插件缺失时 pipeline 会报错, 便于暴露问题)
     *   其他    探测到 mpph26xenc 用硬编, 否则回退软编 (默认) */
    bool hw_available = false;
    GstElementFactory *hw_factory = gst_element_factory_find(enc_hw);
    if (hw_factory)
    {
        hw_available = true;
        gst_object_unref(hw_factory);
    }

    bool use_hw;
    if (g_st.encoder == "hw")
        use_hw = true;
    else
        use_hw = hw_available; /* auto (含旧 "sw" 值不再强制软编) */
    if (use_hw)
    {
        g_st.dma_pool = DmaFramePool::create(g_st.width, g_st.height, g_st.enc_w, g_st.enc_h);
        if (!g_st.dma_pool)
        {
            fprintf(stderr,
                    "[RTSP] hardware DMA path unavailable; falling back to software encoder for portability\n");
            use_hw = false;
        }
    }
    if (!use_hw)
        g_st.dma_pool.reset();
    g_st.use_hw = use_hw;

    char launch[1024];
    if (use_hw)
    {
        /* 硬件编码属性在 media-configure 中按插件实际支持项设置，避免版本差异导致 launch 解析失败。 */
        snprintf(launch, sizeof(launch),
                 "( appsrc name=mysrc ! queue max-size-buffers=4 leaky=downstream "
                 "! video/x-raw,format=NV12 ! %s name=video_encoder "
                 "! %s ! %s name=pay0 pt=96 config-interval=1 )",
                 enc_hw, parse_elem, pay_elem);
        printf("[RTSP] encoder: HW %s (mode=%s, hw_available=%d)\n", enc_hw, g_st.encoder.c_str(),
               hw_available ? 1 : 0);
    }
    else
    {
        const char *enc_sw = h265 ? "x265enc" : "x264enc";
        snprintf(launch, sizeof(launch),
                 "( appsrc name=mysrc ! queue max-size-buffers=4 leaky=downstream "
                 "! videoconvert ! %s name=video_encoder tune=zerolatency speed-preset=ultrafast bitrate=%d "
                 "! %s ! %s name=pay0 pt=96 config-interval=1 )",
                 enc_sw, g_st.bitrate, parse_elem, pay_elem);
        printf("[RTSP] encoder: SW %s (mode=%s, hw_available=%d, %dkbps)\n", enc_sw, g_st.encoder.c_str(),
               hw_available ? 1 : 0, g_st.bitrate);
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
    /* MPP 硬件编码器要求宽高对齐到 16 (1080 这类非 16 对齐高度会吐绿)。
     * 向上对齐, 源画面贴左上, 右/下补黑边。 */
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

    const std::string launch = build_launch_string();
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

    printf("[RTSP] streaming composited view at rtsp://<board-ip>:%d%s "
           "(codec=%s, %dfps, %dx%d)\n",
           g_st.port, g_st.path.c_str(), g_st.codec.c_str(), g_st.fps, g_st.width, g_st.height);
    return 0;
}

int rtsp_streamer_has_active_client(void)
{
    return g_st.client_active.load(std::memory_order_acquire) ? 1 : 0;
}

void rtsp_streamer_deinit(void)
{
    if (!g_st.inited)
        return;

    /* 先停 feeder, 再退服务循环 */
    g_st.feeder_exit.store(true);
    g_st.client_active.store(false, std::memory_order_release);
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
    g_st.dma_pool.reset();

    g_st.inited = false;
    printf("[RTSP] streamer deinit done\n");
}
