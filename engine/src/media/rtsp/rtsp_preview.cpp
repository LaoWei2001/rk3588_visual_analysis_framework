#include "live_source.h"
#include "rtsp_streamer.h"
#include "rtsp_streamer_internal.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <json.h>
#include <stdexcept>

using namespace rtsp_detail;
namespace rtsp_detail
{
void expire_channel_previews_locked();
std::string preview_reply(bool ok, const char *message, const RtspStreamer *state = nullptr, bool supported = false);
} // namespace rtsp_detail

void rtsp_detail::stop_channel_preview(const std::shared_ptr<RtspStreamer> &state)
{
    GstRTSPMedia *media = nullptr;
    {
        std::lock_guard<std::mutex> lifecycle(state->media_mutex);
        state->retired.store(true);
        state->feeder_exit.store(true);
        if (state->preview_client_active.exchange(false))
            g_channel_preview_active[state->channel_id].fetch_sub(1);
        media = GST_RTSP_MEDIA(g_weak_ref_get(&state->media_ref));
        state->media_source.clear();
    }
    if (g_st.server)
    {
        auto *mounts = gst_rtsp_server_get_mount_points(g_st.server);
        gst_rtsp_mount_points_remove_factory(mounts, state->path.c_str());
        g_object_unref(mounts);
    }
    if (state->feeder_running)
    {
        pthread_join(state->feeder_tid, nullptr);
        state->feeder_running = false;
    }
    if (media)
    {
        gst_rtsp_media_unprepare(media);
        g_object_unref(media);
    }
    {
        std::lock_guard<std::mutex> lock(state->frame_mutex);
        state->pending_frame.reset();
    }
    state->dma_pool.reset();
}

// Caller holds g_preview_operations_mutex. Per-frame readers only need the
// map mutex; all slow teardown happens after objects have been removed.
void rtsp_detail::expire_channel_previews_locked()
{
    const uint64_t now = preview_now_ms();
    std::vector<std::shared_ptr<RtspStreamer>> retired;
    {
        std::lock_guard<std::mutex> lock(g_channel_previews_mutex);
        for (auto it = g_channel_previews.begin(); it != g_channel_previews.end();)
        {
            auto &leases = it->second->leases;
            for (auto lease = leases.begin(); lease != leases.end();)
                if (lease->second <= now)
                    lease = leases.erase(lease);
                else
                    ++lease;
            if (leases.empty() || !app_ctrl_has_channel(it->second->channel_id))
            {
                retired.push_back(it->second);
                it = g_channel_previews.erase(it);
            }
            else
                ++it;
        }
    }
    for (const auto &state : retired)
        rtsp_detail::stop_channel_preview(state);
}
gboolean rtsp_detail::expire_channel_previews(gpointer)
{
    // Never hold up the RTSP event loop while another thread unprepares media.
    std::unique_lock<std::mutex> operation(g_preview_operations_mutex, std::try_to_lock);
    if (!operation.owns_lock())
        return G_SOURCE_CONTINUE;
    if (!g_preview_ready.load())
        return G_SOURCE_REMOVE;
    rtsp_detail::expire_channel_previews_locked();
    return G_SOURCE_CONTINUE;
}

int rtsp_streamer_has_channel_client(int channel_id)
{
    return channel_id >= 0 && channel_id < MAX_CHANNEL_NUM && g_preview_ready.load() &&
           g_channel_preview_active[channel_id].load() > 0;
}

void rtsp_streamer_submit_channel_frame(int channel_id, const void *data, int format, int width, int height,
                                        int horizontal_stride, int vertical_stride)
{
    if (!rtsp_streamer_has_channel_client(channel_id) || !data || width <= 0 || height <= 0 ||
        horizontal_stride < width || vertical_stride < height)
        return;
    const uint64_t pixels = uint64_t(horizontal_stride) * uint64_t(vertical_stride);
    uint64_t bytes = 0;
    if (format == RK_FORMAT_YCbCr_420_SP || format == RK_FORMAT_YCrCb_420_SP)
        bytes = pixels * 3 / 2;
    else if (format == RK_FORMAT_RGB_888 || format == RK_FORMAT_BGR_888)
        bytes = pixels * 3;
    if (!bytes || bytes > 64 * 1024 * 1024)
        return;
    std::vector<std::shared_ptr<RtspStreamer>> states;
    {
        std::lock_guard<std::mutex> lock(g_channel_previews_mutex);
        for (const auto &entry : g_channel_previews)
            if (entry.second->channel_id == channel_id && entry.second->preview_client_active.load())
                states.push_back(entry.second);
    }
    if (states.empty())
        return;
    try
    {
        // One immutable latest slot, with at most one older frame held by the
        // encoder worker. Decoding and inference never wait for MPP or RGA here.
        auto input = std::make_shared<ChannelPreviewFrame>();
        input->format = format;
        input->width = width;
        input->height = height;
        input->stride_w = horizontal_stride;
        input->stride_h = vertical_stride;
        input->bytes.resize(static_cast<size_t>(bytes));
        memcpy(input->bytes.data(), data, input->bytes.size());
        for (const auto &state : states)
        {
            std::lock_guard<std::mutex> lock(state->frame_mutex);
            if (!state->retired.load())
                state->pending_frame = input;
        }
    }
    catch (const std::exception &error)
    {
        fprintf(stderr, "[RTSP] channel %d preview frame skipped: %s\n", channel_id, error.what());
    }
}

std::string rtsp_detail::preview_reply(bool ok, const char *message, const RtspStreamer *state, bool supported)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
        return "{\"ok\":false,\"message\":\"out of memory\"}";
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "protocol", "channel-preview-v1");
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddBoolToObject(root, "supported", supported);
    cJSON_AddNumberToObject(root, "max_channels", 4);
    cJSON_AddNumberToObject(root, "max_width", 1920);
    cJSON_AddNumberToObject(root, "max_height", 1080);
    const int resolutions[] = {480, 720, 1080};
    cJSON_AddItemToObject(root, "preview_resolutions", cJSON_CreateIntArray(resolutions, 3));
    if (state)
    {
        cJSON_AddNumberToObject(root, "channel_id", state->channel_id);
        cJSON_AddNumberToObject(root, "width", state->enc_w);
        cJSON_AddNumberToObject(root, "height", state->enc_h);
        cJSON_AddNumberToObject(root, "content_width", state->width);
        cJSON_AddNumberToObject(root, "content_height", state->height);
        cJSON_AddStringToObject(root, "path", state->path.c_str());
        cJSON_AddNumberToObject(root, "port", state->port);
    }
    char *encoded = cJSON_PrintUnformatted(root);
    std::string result = encoded ? encoded : "{\"ok\":false,\"message\":\"out of memory\"}";
    if (encoded)
        cJSON_free(encoded);
    cJSON_Delete(root);
    return result;
}

std::string rtsp_streamer_preview_control(const char *action, int channel_id, const char *lease, int preview_height)
{
    std::lock_guard<std::mutex> operation(g_preview_operations_mutex);
    if (!action)
        return rtsp_detail::preview_reply(false, "Invalid preview action");
    if (strcmp(action, "status") == 0)
    {
        auto *factory = g_preview_ready.load() ? gst_element_factory_find("mpph264enc") : nullptr;
        const bool supported = factory != nullptr;
        if (factory)
            gst_object_unref(factory);
        return rtsp_detail::preview_reply(true, "", nullptr, supported);
    }
    if (!g_preview_ready.load())
        return rtsp_detail::preview_reply(false, "RTSP 预览尚未就绪");
    if (!lease || !*lease || strlen(lease) > 64)
        return rtsp_detail::preview_reply(false, "Invalid preview lease");
    if (preview_height != 480 && preview_height != 720 && preview_height != 1080)
        return rtsp_detail::preview_reply(false, "预览分辨率仅支持 480p、720p、1080p");
    const auto key = std::make_pair(channel_id, preview_height);
    auto found = g_channel_previews.find(key);
    if (strcmp(action, "release") == 0)
    {
        if (found != g_channel_previews.end())
        {
            auto it = found->second->leases.find(lease);
            // The Web transmuxer keeps an idle producer for three seconds.
            if (it != found->second->leases.end())
                it->second = preview_now_ms() + 8000;
        }
        return rtsp_detail::preview_reply(true, "");
    }
    if (strcmp(action, "acquire") != 0)
        return rtsp_detail::preview_reply(false, "Invalid preview action");
    if (!app_ctrl_has_channel(channel_id))
        return rtsp_detail::preview_reply(false, "未知视频通道");
    rtsp_detail::expire_channel_previews_locked();
    found = g_channel_previews.find(key);
    if (found == g_channel_previews.end())
    {
        if (g_channel_previews.size() >= 4)
            return rtsp_detail::preview_reply(false, "高清预览已达 4 路上限，请关闭其他单窗口后重试");
        int width = 0, height = 0;
        pthread_mutex_lock(&g_pCtrl->chn_mtx[channel_id]);
        width = g_pCtrl->channels_state[channel_id].src_w_now;
        height = g_pCtrl->channels_state[channel_id].src_h_now;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[channel_id]);
        if (width <= 0 || height <= 0)
            return rtsp_detail::preview_reply(false, "通道视频尺寸尚未就绪，请稍后重试");
        std::shared_ptr<RtspStreamer> state;
        try
        {
            state = std::make_shared<RtspStreamer>();
            state->channel_id = channel_id;
            state->port = g_st.port;
            state->path = g_st.path + "/channel/" + std::to_string(channel_id) + "/" + std::to_string(preview_height);
            state->fps = constants::PREVIEW_MAX_FPS;
            state->bitrate = 6000;
            state->encoder = "hw";
            state->codec = "h264";
            const double scale =
                std::min(1.0, std::min((preview_height * 16.0 / 9.0) / width, double(preview_height) / height));
            state->width = std::max(16, int(width * scale) / 2 * 2);
            state->height = std::max(16, int(height * scale) / 2 * 2);
            state->enc_w = (state->width + 15) & ~15;
            state->enc_h = (state->height + 15) & ~15;
            const auto launch = build_launch_string(*state);
            if (launch.empty())
                return rtsp_detail::preview_reply(false, "MPP 高清编码或 DMA 缓冲区暂不可用");
            state->leases.emplace(lease, preview_now_ms() + 35000);
            auto *factory = gst_rtsp_media_factory_new();
            gst_rtsp_media_factory_set_launch(factory, launch.c_str());
            gst_rtsp_media_factory_set_shared(factory, TRUE);
            gst_rtsp_media_factory_set_latency(factory, 0);
            g_signal_connect_data(factory, "media-configure", G_CALLBACK(on_media_configure),
                                  new std::shared_ptr<RtspStreamer>(state), release_callback_state, GConnectFlags(0));
            auto *mounts = gst_rtsp_server_get_mount_points(g_st.server);
            gst_rtsp_mount_points_add_factory(mounts, state->path.c_str(), factory);
            g_object_unref(mounts);
            if (pthread_create(&state->feeder_tid, nullptr, rtsp_feeder_thread, state.get()) != 0)
                throw std::runtime_error("高清预览线程启动失败");
            state->feeder_running = true;
            {
                std::lock_guard<std::mutex> lock(g_channel_previews_mutex);
                found = g_channel_previews.emplace(key, state).first;
            }
        }
        catch (const std::exception &error)
        {
            if (state)
                rtsp_detail::stop_channel_preview(state);
            return rtsp_detail::preview_reply(false, error.what());
        }
    }
    auto &state = *found->second;
    if (state.leases.size() >= 64 && !state.leases.count(lease))
        return rtsp_detail::preview_reply(false, "高清预览观看会话已满");
    state.leases[lease] = preview_now_ms() + 35000;
    return rtsp_detail::preview_reply(true, "", &state, true);
}
