#include "live_source.h"
#include "media/display/display.h"
#include "media/display/display_render.h"
#include "rtsp_streamer.h"
#include "rtsp_streamer_internal.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <thread>
#include <unistd.h>

namespace rtsp_detail
{
RtspStreamer &callback_state(gpointer data)
{
    return data ? **static_cast<std::shared_ptr<RtspStreamer> *>(data) : g_st;
}
void release_callback_state(gpointer data, GClosure *)
{
    delete static_cast<std::shared_ptr<RtspStreamer> *>(data);
}

/*======================== media 生命周期回调 ========================*/

void set_encoder_int_if_supported(GstElement *encoder, const char *property, int value)
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
    else if (G_VALUE_HOLDS_ENUM(&typed_value))
        g_value_set_enum(&typed_value, value);
    else
    {
        g_value_unset(&typed_value);
        return;
    }
    g_object_set_property(G_OBJECT(encoder), property, &typed_value);
    g_value_unset(&typed_value);
}

/* 最后一个客户端断开 → 共享 media 反配置: 清掉 appsrc, feeder 随之空转。 */
void on_media_unprepared(GstRTSPMedia *media, gpointer user_data)
{
    auto &state = callback_state(user_data);
    std::lock_guard<std::mutex> lifecycle(state.media_mutex);
    if (state.media_source.release(media))
    {
        if (state.channel_id >= 0 && state.preview_client_active.exchange(false))
            g_channel_preview_active[state.channel_id].fetch_sub(1);
        printf("[RTSP] media unprepared (no clients)\n");
    }
    else
        printf("[RTSP] retired media unprepared; current source preserved\n");
}

/* 客户端连接、media 创建时调用: 取出 appsrc, 设 caps/属性, 存给 feeder。 */
void on_media_configure(GstRTSPMediaFactory *factory, GstRTSPMedia *media, gpointer user_data)
{
    (void)factory;
    auto &state = callback_state(user_data);

    std::lock_guard<std::mutex> lifecycle(state.media_mutex);
    if (state.retired.load())
        return;
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
        set_encoder_int_if_supported(encoder, "gop", std::max(1, state.fps));
        set_encoder_int_if_supported(encoder, "key-int-max", std::max(1, state.fps));
        set_encoder_int_if_supported(encoder, "bps", std::max(1, state.bitrate) * 1000);
        if (state.channel_id >= 0)
        {
            const int64_t blocks = int64_t((state.enc_w + 15) / 16) * ((state.enc_h + 15) / 16);
            set_encoder_int_if_supported(encoder, "level", blocks > 8192 || blocks * state.fps > 245760 ? 51 : 40);
        }
        gst_object_unref(encoder);
    }

    /* 非阻塞 appsrc 的 max-bytes 只触发 enough-data，不会自动丢帧。
     * feeder 同时检查实际积压，禁止继续灌入旧画面。 */
    const guint64 frame_bytes =
        state.use_hw ? (guint64)state.enc_w * state.enc_h * 3 / 2 : (guint64)state.enc_w * state.enc_h * 3;
    g_object_set(G_OBJECT(appsrc), "format", GST_FORMAT_TIME, "is-live", TRUE, "do-timestamp", TRUE, "block", FALSE,
                 "max-bytes", frame_bytes, nullptr);

    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, state.use_hw ? "NV12" : "RGB", "width",
                                        G_TYPE_INT, state.enc_w, "height", G_TYPE_INT, state.enc_h, "framerate",
                                        GST_TYPE_FRACTION, state.fps, 1, nullptr);
    g_object_set(G_OBJECT(appsrc), "caps", caps, nullptr);
    gst_caps_unref(caps);

    auto *lifetime = user_data
                         ? new std::shared_ptr<RtspStreamer>(*static_cast<std::shared_ptr<RtspStreamer> *>(user_data))
                         : nullptr;
    g_signal_connect_data(media, "unprepared", G_CALLBACK(on_media_unprepared), lifetime, release_callback_state,
                          GConnectFlags(0));
    g_weak_ref_set(&state.media_ref, G_OBJECT(media));
    state.media_source.replace(media, appsrc);
    if (state.channel_id >= 0 && !state.preview_client_active.exchange(true))
        g_channel_preview_active[state.channel_id].fetch_add(1);
    gst_object_unref(appsrc);
    printf("[RTSP] client connected, media configured (%dx%d @%dfps)\n", state.enc_w, state.enc_h, state.fps);
}

/*======================== 推帧线程 ========================*/

void *rtsp_feeder_thread(void *arg)
{
    auto &state = arg ? *static_cast<RtspStreamer *>(arg) : g_st;
    const int src_w = state.width;
    const int src_h = state.height;
    const int dst_w = state.enc_w;
    const int dst_h = state.enc_h;
    const size_t src_stride = (size_t)src_w * 3; /* RGB packed */
    const size_t dst_stride = (size_t)dst_w * 3;
    const size_t rgb_frame_bytes = dst_stride * (size_t)dst_h;
    const auto period = std::chrono::microseconds(1000000 / std::max(1, state.fps));
    auto next_wakeup = std::chrono::steady_clock::now();
    const std::shared_ptr<DmaFramePool> dma_pool = state.dma_pool;
    GstAllocator *dmabuf_allocator = state.use_hw ? gst_dmabuf_allocator_new() : nullptr;

    auto push_buffer = [&state](GstElement *appsrc, GstBuffer *buffer) {
        // 时间戳仍由实时源时钟产生，丢帧后的时间缺口不能压缩成慢动作。
        GST_BUFFER_DURATION(buffer) = GST_SECOND / std::max(1, state.fps);
        GstFlowReturn ret = GST_FLOW_OK;
        g_signal_emit_by_name(appsrc, "push-buffer", buffer, &ret);
        if (ret != GST_FLOW_OK)
        {
            static std::atomic<unsigned> warn_cnt{0};
            if ((warn_cnt++ % 200) == 0)
                fprintf(stderr, "[RTSP] push-buffer ret=%d\n", (int)ret);
        }
    };

    cv::Mat channel_staging, channel_view;
    std::shared_ptr<const ChannelPreviewFrame> rendered_frame;
    uint64_t rendered_at = 0;
    auto copy_rgb = [&](unsigned char *destination, int width, int height) {
        const bool composite = state.channel_id < 0;
        if (composite && !display_try_lock())
            return false;
        const unsigned char *pixels =
            composite ? reinterpret_cast<const unsigned char *>(g_pCtrl->pDispBuffer ? *g_pCtrl->pDispBuffer : nullptr)
                      : channel_view.data;
        if (!pixels)
        {
            if (composite)
                display_unlock();
            return false;
        }
        const size_t input_step = composite ? src_stride : channel_view.step;
        const size_t output_step = static_cast<size_t>(width) * 3;
        if (width == src_w && height == src_h && input_step == output_step)
            memcpy(destination, pixels, output_step * height);
        else
        {
            for (int row = 0; row < src_h; ++row)
            {
                memcpy(destination + row * output_step, pixels + row * input_step, src_stride);
                if (width > src_w)
                    memset(destination + row * output_step + src_stride, 0, output_step - src_stride);
            }
            if (height > src_h)
                memset(destination + src_h * output_step, 0, (height - src_h) * output_step);
        }
        if (composite)
            display_unlock();
        return true;
    };

    while (g_pCtrl && g_pCtrl->isRunning.load() && !state.feeder_exit.load())
    {
        GstElement *src = state.media_source.acquire();

        const guint64 frame_bytes = state.use_hw ? (guint64)dst_w * dst_h * 3 / 2 : (guint64)rgb_frame_bytes;
        if (src && rtsp_source_has_room(src, frame_bytes))
        {
            if (state.channel_id >= 0)
            {
                std::shared_ptr<const ChannelPreviewFrame> input;
                {
                    std::lock_guard<std::mutex> lock(state.frame_mutex);
                    input = state.pending_frame;
                }
                const uint64_t now = preview_now_ms();
                if (input && (input != rendered_frame || now - rendered_at >= 250))
                {
                    try
                    {
                        channel_view.release();
                        if (display_prepare_source_frame(state.channel_id, input->bytes.data(), input->format,
                                                         input->width, input->height, input->stride_w, input->stride_h,
                                                         src_w, src_h, channel_staging))
                        {
                            channel_view = channel_staging(cv::Rect(0, 0, src_w, src_h));
                            render_channel_view(channel_view, state.channel_id);
                            const auto runtime = app_ctrl_get_runtime_snapshot();
                            const auto *channel = app_ctrl_runtime_channel_config(runtime, state.channel_id);
                            if (!channel || !channel->swap_rb)
                                cv::cvtColor(channel_view, channel_view, cv::COLOR_BGR2RGB);
                            if (input != rendered_frame && !app_ctrl_get_enable_disp() &&
                                !rtsp_streamer_has_active_client())
                                g_pCtrl->channels_state[state.channel_id].preview_rate.tick();
                            rendered_frame = input;
                            rendered_at = now;
                        }
                    }
                    catch (const std::exception &error)
                    {
                        channel_view.release();
                        fprintf(stderr, "[RTSP] channel %d render failed: %s\n", state.channel_id, error.what());
                    }
                }
            }
            const bool has_frame = state.channel_id < 0 || !channel_view.empty();
            if (has_frame && state.use_hw && dma_pool && dmabuf_allocator)
            {
                auto output = dma_pool->acquire_output();
                bool frame_ready = output && dma_pool->begin_rgb_write();
                if (frame_ready)
                {
                    frame_ready = copy_rgb(static_cast<unsigned char *>(dma_pool->rgb_source->mapped),
                                           dma_pool->rgb_source->stride_w, dma_pool->rgb_source->stride_h);
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
            else if (has_frame && !state.use_hw)
            {
                GstBuffer *buffer = gst_buffer_new_allocate(nullptr, rgb_frame_bytes, nullptr);
                GstMapInfo map;
                bool frame_ready = buffer && gst_buffer_map(buffer, &map, GST_MAP_WRITE);
                if (frame_ready)
                {
                    frame_ready = copy_rgb(map.data, dst_w, dst_h);
                    gst_buffer_unmap(buffer, &map);
                }
                if (frame_ready)
                    push_buffer(src, buffer);
                if (buffer)
                    gst_buffer_unref(buffer);
            }
        }
        if (src)
            gst_object_unref(src);
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
/*======================== 编码管线选择 ========================*/

/* 运行时探测硬件编码器: 有 mpph26xenc 用硬编, 否则回退 x26xenc 软编。
 * 注意 launch 字符串需用 ( ) 包裹, 且 payloader 必须命名为 pay0。*/
std::string build_launch_string(RtspStreamer &state)
{
    const bool h265 = (state.codec == "h265" || state.codec == "hevc");
    const char *enc_hw = h265 ? "mpph265enc" : "mpph264enc";
    const char *parse_elem = h265 ? "h265parse" : "h264parse";
    const char *pay_elem = h265 ? "rtph265pay" : "rtph264pay";

    /* "hw" 强制硬件编码；"auto" 优先硬件，不可用时回退软件编码。 */
    bool hw_available = false;
    GstElementFactory *hw_factory = gst_element_factory_find(enc_hw);
    if (hw_factory)
    {
        hw_available = true;
        gst_object_unref(hw_factory);
    }

    // Optional HD views must not silently consume CPU with a software encoder.
    if (state.channel_id >= 0 && !hw_available)
        return {};
    bool use_hw;
    if (state.encoder == "hw")
        use_hw = true;
    else
        use_hw = hw_available;
    if (use_hw)
    {
        state.dma_pool = DmaFramePool::create(state.width, state.height, state.enc_w, state.enc_h);
        if (!state.dma_pool)
        {
            if (state.channel_id >= 0)
                return {};
            fprintf(stderr, "[RTSP] hardware DMA path unavailable; falling back to software encoder for portability\n");
            use_hw = false;
        }
    }
    if (!use_hw)
        state.dma_pool.reset();
    state.use_hw = use_hw;

    char launch[1024];
    if (use_hw)
    {
        /* 硬件编码属性在 media-configure 中按插件实际支持项设置，避免版本差异导致 launch 解析失败。 */
        snprintf(launch, sizeof(launch),
                 "( appsrc name=mysrc ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream "
                 "! video/x-raw,format=NV12 ! %s name=video_encoder "
                 "! %s ! %s name=pay0 pt=96 config-interval=1 )",
                 enc_hw, parse_elem, pay_elem);
        printf("[RTSP] encoder: HW %s (mode=%s, hw_available=%d)\n", enc_hw, state.encoder.c_str(),
               hw_available ? 1 : 0);
    }
    else
    {
        const char *enc_sw = h265 ? "x265enc" : "x264enc";
        snprintf(launch, sizeof(launch),
                 "( appsrc name=mysrc ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream "
                 "! videoconvert ! %s name=video_encoder tune=zerolatency speed-preset=ultrafast bitrate=%d "
                 "! %s ! %s name=pay0 pt=96 config-interval=1 )",
                 enc_sw, state.bitrate, parse_elem, pay_elem);
        printf("[RTSP] encoder: SW %s (mode=%s, hw_available=%d, %dkbps)\n", enc_sw, state.encoder.c_str(),
               hw_available ? 1 : 0, state.bitrate);
    }
    return std::string(launch);
}
} // namespace rtsp_detail
