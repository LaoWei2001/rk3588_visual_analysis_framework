#include "event_video_recorder.h"

#include "../event/event_report.h"
#include "../analyzer/algoProcess.h"
#include "../analyzer/frame_pipeline.h"
#include "../config/config.h"
#include "../core/app_ctrl.h"
#include "../player/display.h"

#include <gst/gst.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <rga/RgaApi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <pthread.h>
#include <queue>
#include <utility>
#include <vector>

namespace
{

struct RawFrame
{
    int channel_id = -1;
    int format = 0;
    int width = 0, height = 0, stride_w = 0, stride_h = 0;
    uint64_t timestamp_ms = 0;
    float pre_sec = 5.0f;
    EventVideoOverlayMode overlay_mode = EVENT_VIDEO_OVERLAY_NONE;
    std::vector<unsigned char> bytes;

    /* 叠加信息必须和源帧同时快照。录像转换在线程池异步执行，如果届时再读取
     * ChannelState，目标可能已经离场或结果已被下一帧覆盖，最终就会只剩文字没有框。 */
    int input_w = 0, input_h = 0;
    float disp_fps = 0.0f;
    uint64_t result_ts_ms = 0;
    bool swap_rb = false;
    std::vector<RoiZone> rois;
    std::vector<AlgoResult> results;
    std::vector<DrawCommand> commands;
};

struct FrameSample
{
    uint64_t timestamp_ms = 0;
    int width = 0, height = 0;
    std::shared_ptr<std::vector<unsigned char>> jpeg;
};

struct ChannelRing
{
    std::deque<FrameSample> frames;
    uint64_t last_capture_ms = 0;
};

struct VideoEvent
{
    EventVideoRequest request;
    uint64_t trigger_ms = 0;
    uint64_t start_ms = 0;
    uint64_t end_ms = 0;
    std::vector<FrameSample> frames;
    size_t encoded_frames = 0;
    double encoded_duration_sec = 0.0;
    double available_pre_sec = 0.0;
    double available_post_sec = 0.0;
};

static ChannelRing g_rings[MAX_CHANNEL_NUM];
/* event_id -> 正在收集的录像。不同告警的时间窗允许重叠，不能因为同通道同类型
 * 新事件到来就提前截断上一个事件。 */
static std::map<std::string, VideoEvent> g_active;
/* channel:event_type -> 最近事件ID，仅用于5秒合并事件的 post 窗口延长。 */
static std::map<std::string, std::string> g_latest_by_key;
static std::queue<RawFrame> g_raw_queues[MAX_CHANNEL_NUM];
static std::queue<VideoEvent> g_video_jobs;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static constexpr size_t WORKER_COUNT = 3;
/* Raw 1080p frames are large. A short queue absorbs jitter without allowing
 * every channel to retain tens of megabytes indefinitely. */
static constexpr size_t CHANNEL_QUEUE_CAPACITY = 4;
static constexpr size_t MAX_CONCURRENT_ENCODERS = 1;
static pthread_t g_worker_tids[WORKER_COUNT];
static size_t g_worker_count = 0;
static bool g_channel_busy[MAX_CHANNEL_NUM] = {false};
static size_t g_next_channel = 0;
static size_t g_active_encoders = 0;
static bool g_started = false;
static bool g_running = false;

static uint64_t now_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

static int clamp_fps(int value)
{
    return std::max(1, std::min(30, value));
}
static float clamp_sec(float value)
{
    return std::max(0.0f, std::min(120.0f, value));
}
static std::string key_for(int channel_id, const std::string &type)
{
    return std::to_string(channel_id) + ":" + type;
}

static size_t raw_size(int format, int stride_w, int stride_h)
{
    if (format == RK_FORMAT_YCbCr_420_SP || format == RK_FORMAT_YCrCb_420_SP)
        return static_cast<size_t>(stride_w) * stride_h * 3 / 2;
    if (format == RK_FORMAT_BGR_888 || format == RK_FORMAT_RGB_888)
        return static_cast<size_t>(stride_w) * stride_h * 3;
    return 0;
}

static bool raw_to_bgr(const RawFrame &raw, cv::Mat &bgr)
{
    if (raw.bytes.empty() || raw.width <= 0 || raw.height <= 0)
        return false;
    const int aligned_w = (raw.width + 15) & ~15;
    cv::Mat staging(raw.height, aligned_w, CV_8UC3);
    RgaImage src_img{static_cast<RgaSURF_FORMAT>(raw.format),      raw.width, raw.height, raw.stride_w, raw.stride_h, 0,
                     const_cast<unsigned char *>(raw.bytes.data())};
    RgaImage dst_img{RK_FORMAT_BGR_888, raw.width, raw.height, aligned_w, raw.height, 0, staging.data};
    if (rga_convert_resize(raw.channel_id, src_img, dst_img))
    {
        bgr = staging(cv::Rect(0, 0, raw.width, raw.height));
        return true;
    }

    /* RGA 在不支持的板卡/格式上失败时保留软件回退。 */
    if (raw.format == RK_FORMAT_YCbCr_420_SP || raw.format == RK_FORMAT_YCrCb_420_SP)
    {
        cv::Mat yuv(raw.stride_h * 3 / 2, raw.stride_w, CV_8UC1, const_cast<unsigned char *>(raw.bytes.data()));
        cv::Mat full;
        cv::cvtColor(yuv, full, raw.format == RK_FORMAT_YCbCr_420_SP ? cv::COLOR_YUV2BGR_NV12 : cv::COLOR_YUV2BGR_NV21);
        bgr = full(cv::Rect(0, 0, raw.width, raw.height));
        return true;
    }
    if (raw.format == RK_FORMAT_BGR_888 || raw.format == RK_FORMAT_RGB_888)
    {
        cv::Mat full(raw.stride_h, raw.stride_w, CV_8UC3, const_cast<unsigned char *>(raw.bytes.data()));
        cv::Mat visible = full(cv::Rect(0, 0, raw.width, raw.height));
        if (raw.format == RK_FORMAT_RGB_888)
            cv::cvtColor(visible, bgr, cv::COLOR_RGB2BGR);
        else
            bgr = visible;
        return true;
    }
    return false;
}

static void render_video_overlays(const RawFrame &raw, cv::Mat &bgr)
{
    if (raw.overlay_mode == EVENT_VIDEO_OVERLAY_NONE || !g_pCtrl || bgr.empty())
        return;

    if (raw.overlay_mode == EVENT_VIDEO_OVERLAY_DISPLAY)
    {
        const int output_width = tile_width(raw.channel_id);
        const int output_height = tile_height(raw.channel_id);
        if (output_width <= 0 || output_height <= 0)
            return;
        if (bgr.cols != output_width || bgr.rows != output_height)
            cv::resize(bgr, bgr, cv::Size(output_width, output_height));
        RenderParams params;
        params.chnId = raw.channel_id;
        params.inputW = raw.input_w;
        params.inputH = raw.input_h;
        if (params.inputW <= 0 || params.inputH <= 0)
            return;
        params.disp_fps = raw.disp_fps;
        params.infer_fps = algorithm_get_infer_fps(raw.channel_id);
        params.result_age_ms = raw.result_ts_ms && raw.timestamp_ms >= raw.result_ts_ms
                                   ? static_cast<int64_t>(std::min<uint64_t>(raw.timestamp_ms - raw.result_ts_ms, 200))
                                   : 0;
        /* 视频自动复用实时画面的绘制层，并保留 VIDEO 专用绘制能力。 */
        params.target_mask = static_cast<uint8_t>(DrawCommand::DISPLAY | DrawCommand::VIDEO);
        params.show_fps = 0;
        params.show_system_overlays = true;
        params.show_custom_overlays = true;
        params.roi_zones = &raw.rois;
        params.results = &raw.results;
        params.draw_cmds = &raw.commands;
        render_overlays(bgr, params);

        if (raw.swap_rb)
            cv::cvtColor(bgr, bgr, cv::COLOR_BGR2RGB);
        return;
    }

    RenderParams params;
    params.chnId = raw.channel_id;
    params.inputW = raw.input_w;
    params.inputH = raw.input_h;
    if (params.inputW <= 0 || params.inputH <= 0)
        return;
    params.disp_fps = raw.disp_fps;
    params.infer_fps = algorithm_get_infer_fps(raw.channel_id);
    params.result_age_ms = raw.result_ts_ms && raw.timestamp_ms >= raw.result_ts_ms
                               ? static_cast<int64_t>(std::min<uint64_t>(raw.timestamp_ms - raw.result_ts_ms, 200))
                               : 0;
    params.show_fps = 0;
    params.target_mask = DrawCommand::VIDEO;
    params.show_system_overlays = raw.overlay_mode == EVENT_VIDEO_OVERLAY_ALL;
    params.show_custom_overlays = true;
    params.roi_zones = &raw.rois;
    params.results = &raw.results;
    params.draw_cmds = &raw.commands;
    render_overlays(bgr, params);
}

static bool event_has_pending_frame_locked(const VideoEvent &event)
{
    const int channel_id = event.request.channel_id;
    if (channel_id < 0 || channel_id >= MAX_CHANNEL_NUM)
        return false;
    /* 正在锁外转换的帧也必须等完，否则封口后该帧无法再进入事件。 */
    if (g_channel_busy[channel_id])
        return true;
    const auto &queue = g_raw_queues[channel_id];
    /* 每通道队列按时间递增；队首已经晚于窗口终点时，后续帧也无需再等待。 */
    return !queue.empty() && queue.front().timestamp_ms <= event.end_ms;
}

static void expire_locked(uint64_t now, bool force)
{
    for (auto it = g_active.begin(); it != g_active.end();)
    {
        VideoEvent &event = it->second;
        if (force || (now >= event.end_ms && !event_has_pending_frame_locked(event)))
        {
            const std::string merge_key = key_for(event.request.channel_id, event.request.event_type);
            auto latest = g_latest_by_key.find(merge_key);
            if (latest != g_latest_by_key.end() && latest->second == event.request.event_id)
                g_latest_by_key.erase(latest);
            g_video_jobs.push(std::move(event));
            it = g_active.erase(it);
        }
        else
            ++it;
    }
}

static void expire_locked(uint64_t now)
{
    expire_locked(now, false);
}

static void process_raw(RawFrame &raw)
{
    cv::Mat bgr;
    if (!raw_to_bgr(raw, bgr))
        return;
    render_video_overlays(raw, bgr);
    FrameSample sample;
    sample.timestamp_ms = raw.timestamp_ms;
    sample.width = bgr.cols;
    sample.height = bgr.rows;
    sample.jpeg = std::make_shared<std::vector<unsigned char>>();
    if (!cv::imencode(".jpg", bgr, *sample.jpeg, {cv::IMWRITE_JPEG_QUALITY, 85}))
        return;

    pthread_mutex_lock(&g_mtx);
    ChannelRing &ring = g_rings[raw.channel_id];
    ring.frames.push_back(sample);
    const uint64_t keep = raw.timestamp_ms > static_cast<uint64_t>(raw.pre_sec * 1000.0f)
                              ? raw.timestamp_ms - static_cast<uint64_t>(raw.pre_sec * 1000.0f)
                              : 0;
    while (!ring.frames.empty() && ring.frames.front().timestamp_ms < keep)
        ring.frames.pop_front();
    for (auto &entry : g_active)
    {
        VideoEvent &event = entry.second;
        if (event.request.channel_id == raw.channel_id && raw.timestamp_ms >= event.start_ms &&
            raw.timestamp_ms <= event.end_ms)
            event.frames.push_back(sample);
    }
    expire_locked(raw.timestamp_ms);
    pthread_mutex_unlock(&g_mtx);
}

static bool encoder_available(const char *name)
{
    GstElementFactory *factory = gst_element_factory_find(name);
    if (!factory)
        return false;
    gst_object_unref(factory);
    return true;
}

static bool write_h264_mp4(VideoEvent &event, const char *encoder)
{
    if (event.frames.empty() || event.request.output_path.empty())
        return false;

    /* MP4 是恒定帧率容器。异步转换队列在高负载时可能丢采样帧，不能简单地把
     * “实际保留下来的帧数”按配置 FPS 连续写入，否则6秒真实窗口会被压成3秒。
     * 这里先按源时间戳整理，再在真实可用时间范围上重采样到配置 FPS：
     *   - 样本不足时重复最近一帧，时长不被压缩；
     *   - 视频开头/结尾没有画面时，只编码实际存在的范围，不伪造前后内容。 */
    std::sort(event.frames.begin(), event.frames.end(),
              [](const FrameSample &a, const FrameSample &b) { return a.timestamp_ms < b.timestamp_ms; });
    event.frames.erase(std::remove_if(event.frames.begin(), event.frames.end(),
                                      [&](const FrameSample &sample) {
                                          return sample.timestamp_ms < event.start_ms ||
                                                 sample.timestamp_ms > event.end_ms;
                                      }),
                       event.frames.end());
    if (event.frames.empty())
        return false;

    const cv::Size size(event.frames.front().width, event.frames.front().height);
    const int fps = clamp_fps(event.request.fps);
    const uint64_t first_ms = event.frames.front().timestamp_ms;
    const uint64_t last_ms = event.frames.back().timestamp_ms;
    const uint64_t span_ms = last_ms >= first_ms ? last_ms - first_ms : 0;
    const uint64_t output_frame_count = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::llround(static_cast<double>(span_ms) * static_cast<double>(fps) / 1000.0)) + 1);

    event.available_pre_sec =
        first_ms < event.trigger_ms
            ? std::min<double>(event.request.pre_sec, static_cast<double>(event.trigger_ms - first_ms) / 1000.0)
            : 0.0;
    event.available_post_sec =
        last_ms > event.trigger_ms
            ? std::min<double>(event.request.post_sec, static_cast<double>(last_ms - event.trigger_ms) / 1000.0)
            : 0.0;
    event.encoded_duration_sec = static_cast<double>(output_frame_count) / static_cast<double>(fps);
    const char *raw_format = strcmp(encoder, "mpph264enc") == 0 ? "NV12" : "I420";
    char launch[2048];
    snprintf(launch, sizeof(launch),
             "appsrc name=video_source is-live=false block=true format=time "
             "! queue max-size-buffers=8 "
             "! videoconvert ! video/x-raw,format=%s "
             "! %s ! h264parse "
             "! video/x-h264,stream-format=avc,alignment=au "
             "! mp4mux faststart=true "
             "! filesink location=\"%s\"",
             raw_format, encoder, event.request.output_path.c_str());

    GError *parse_error = nullptr;
    GstElement *pipeline = gst_parse_launch(launch, &parse_error);
    if (!pipeline)
    {
        fprintf(stderr, "[event_video] cannot create H264 pipeline encoder=%s: %s\n", encoder,
                parse_error ? parse_error->message : "unknown");
        if (parse_error)
            g_error_free(parse_error);
        return false;
    }
    if (parse_error)
        g_error_free(parse_error);

    GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "video_source");
    if (!source)
    {
        gst_object_unref(pipeline);
        return false;
    }
    GstCaps *caps =
        gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGR", "width", G_TYPE_INT, size.width, "height",
                            G_TYPE_INT, size.height, "framerate", GST_TYPE_FRACTION, fps, 1, nullptr);
    g_object_set(G_OBJECT(source), "caps", caps, nullptr);
    gst_caps_unref(caps);

    bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    const GstClockTime frame_duration = gst_util_uint64_scale_int(GST_SECOND, 1, fps);
    uint64_t frame_index = 0;
    size_t source_index = 0;
    size_t decoded_index = static_cast<size_t>(-1);
    cv::Mat decoded_frame;
    for (uint64_t output_index = 0; output_index < output_frame_count; ++output_index)
    {
        if (!ok)
            break;
        const uint64_t target_ms =
            first_ms +
            static_cast<uint64_t>(std::llround(static_cast<double>(output_index) * 1000.0 / static_cast<double>(fps)));
        while (source_index + 1 < event.frames.size() && event.frames[source_index + 1].timestamp_ms <= target_ms)
            ++source_index;

        if (decoded_index != source_index)
        {
            if (!event.frames[source_index].jpeg)
                continue;
            decoded_frame = cv::imdecode(*event.frames[source_index].jpeg, cv::IMREAD_COLOR);
            if (decoded_frame.empty())
                continue;
            if (decoded_frame.size() != size)
                cv::resize(decoded_frame, decoded_frame, size);
            if (!decoded_frame.isContinuous())
                decoded_frame = decoded_frame.clone();
            decoded_index = source_index;
        }

        const size_t bytes = decoded_frame.total() * decoded_frame.elemSize();
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, bytes, nullptr);
        if (!buffer || gst_buffer_fill(buffer, 0, decoded_frame.data, bytes) != bytes)
        {
            if (buffer)
                gst_buffer_unref(buffer);
            ok = false;
            break;
        }
        GST_BUFFER_PTS(buffer) = frame_index * frame_duration;
        GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DURATION(buffer) = frame_duration;
        ++frame_index;

        GstFlowReturn flow = GST_FLOW_OK;
        g_signal_emit_by_name(source, "push-buffer", buffer, &flow);
        gst_buffer_unref(buffer);
        if (flow != GST_FLOW_OK)
            ok = false;
    }

    GstFlowReturn eos_flow = GST_FLOW_OK;
    if (ok)
        g_signal_emit_by_name(source, "end-of-stream", &eos_flow);
    if (eos_flow != GST_FLOW_OK)
        ok = false;

    if (ok)
    {
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *message =
            gst_bus_timed_pop_filtered(bus, 30 * GST_SECOND, (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (!message || GST_MESSAGE_TYPE(message) != GST_MESSAGE_EOS)
        {
            ok = false;
            if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
            {
                GError *error = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                fprintf(stderr, "[event_video] H264 encode failed encoder=%s: %s\n", encoder,
                        error ? error->message : "unknown");
                if (error)
                    g_error_free(error);
                if (debug)
                    g_free(debug);
            }
        }
        if (message)
            gst_message_unref(message);
        gst_object_unref(bus);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(source);
    gst_object_unref(pipeline);
    event.encoded_frames = static_cast<size_t>(frame_index);
    return ok && frame_index > 0;
}

static bool write_video(VideoEvent &event)
{
    if (event.frames.empty())
    {
        event_report_video_failed(event.request.event_id, event.request.output_path,
                                  "no frames available in requested event window");
        return false;
    }
    if (event.request.output_path.empty())
    {
        event_report_video_failed(event.request.event_id, event.request.output_path,
                                  "video output path is empty");
        return false;
    }

    static const char *encoders[] = {"mpph264enc", "x264enc", "openh264enc"};
    const char *used_encoder = nullptr;
    for (const char *encoder : encoders)
    {
        if (!encoder_available(encoder))
            continue;
        ::remove(event.request.output_path.c_str());
        if (write_h264_mp4(event, encoder))
        {
            used_encoder = encoder;
            break;
        }
    }
    if (!used_encoder)
    {
        fprintf(stderr, "[event_video] no working H264 encoder for: %s\n", event.request.output_path.c_str());
        ::remove(event.request.output_path.c_str());
        event_report_video_failed(event.request.event_id, event.request.output_path,
                                  "no working H264 encoder or video encoding failed");
        return false;
    }

    event_report_video_ready(event.request.event_id, event.request.output_path);
    printf("[event_video] ready event=%s sampled=%zu encoded=%zu duration=%.2fs "
           "pre=%.2f/%.2fs post=%.2f/%.2fs size=%dx%d codec=h264 encoder=%s\n",
           event.request.event_id.c_str(), event.frames.size(), event.encoded_frames, event.encoded_duration_sec,
           event.available_pre_sec, event.request.pre_sec, event.available_post_sec, event.request.post_sec,
           event.frames.front().width, event.frames.front().height, used_encoder);
    return true;
}

static bool raw_queues_empty_locked()
{
    for (int channel_id = 0; channel_id < MAX_CHANNEL_NUM; ++channel_id)
        if (!g_raw_queues[channel_id].empty())
            return false;
    return true;
}

static bool channels_busy_locked()
{
    for (int channel_id = 0; channel_id < MAX_CHANNEL_NUM; ++channel_id)
        if (g_channel_busy[channel_id])
            return true;
    return false;
}

static bool pop_raw_locked(RawFrame &raw, int &channel_id)
{
    for (size_t offset = 0; offset < MAX_CHANNEL_NUM; ++offset)
    {
        const size_t index = (g_next_channel + offset) % MAX_CHANNEL_NUM;
        if (g_channel_busy[index] || g_raw_queues[index].empty())
            continue;
        raw = std::move(g_raw_queues[index].front());
        g_raw_queues[index].pop();
        g_channel_busy[index] = true;
        g_next_channel = (index + 1) % MAX_CHANNEL_NUM;
        channel_id = static_cast<int>(index);
        return true;
    }
    return false;
}

static void *worker_main(void *)
{
    while (true)
    {
        RawFrame raw;
        VideoEvent video;
        int raw_channel = -1;
        bool have_raw = false, have_video = false;
        pthread_mutex_lock(&g_mtx);
        expire_locked(now_ms());
        /* 退出时先让所有已入队源帧完成转换，再截断仍未到计划终点的事件。
         * 这样源视频提前结束时会得到“实际可用的前/后窗口”，不会等待或伪造画面，
         * 也不会像旧实现那样先封口、再把队列中的最后几帧白白处理掉。 */
        if (!g_running && raw_queues_empty_locked() && !channels_busy_locked() && !g_active.empty())
            expire_locked(now_ms(), true);
        /* MP4 encoding is limited to one worker. Other workers keep consuming
         * per-channel source queues, and a channel is processed in order. */
        if (!g_video_jobs.empty() && g_active_encoders < MAX_CONCURRENT_ENCODERS)
        {
            video = std::move(g_video_jobs.front());
            g_video_jobs.pop();
            ++g_active_encoders;
            have_video = true;
        }
        else if (pop_raw_locked(raw, raw_channel))
        {
            have_raw = true;
        }
        else if (!g_running && raw_queues_empty_locked() && g_active.empty() && g_video_jobs.empty() &&
                 g_active_encoders == 0)
        {
            pthread_mutex_unlock(&g_mtx);
            break;
        }
        else
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 200000000;
            if (ts.tv_nsec >= 1000000000)
            {
                ++ts.tv_sec;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&g_cv, &g_mtx, &ts);
        }
        pthread_mutex_unlock(&g_mtx);
        if (have_raw)
        {
            process_raw(raw);
            pthread_mutex_lock(&g_mtx);
            g_channel_busy[raw_channel] = false;
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mtx);
        }
        else if (have_video)
        {
            write_video(video);
            pthread_mutex_lock(&g_mtx);
            --g_active_encoders;
            pthread_cond_broadcast(&g_cv);
            pthread_mutex_unlock(&g_mtx);
        }
    }
    return nullptr;
}

static bool ensure_started_locked()
{
    if (g_started)
        return g_running;
    g_running = true;
    g_worker_count = 0;
    for (size_t i = 0; i < WORKER_COUNT; ++i)
    {
        if (pthread_create(&g_worker_tids[g_worker_count], nullptr, worker_main, nullptr) == 0)
            ++g_worker_count;
        else
            fprintf(stderr, "[event_video] failed to create worker %zu\n", i);
    }
    if (g_worker_count == 0)
    {
        g_running = false;
        return false;
    }
    g_started = true;
    return true;
}

} // namespace

void event_video_recorder_push_source_frame(int channel_id, const void *data, int format, int width, int height,
                                            int stride_w, int stride_h, int fps, float pre_sec,
                                            EventVideoOverlayMode overlay_mode)
{
    if (!data || !app_ctrl_has_channel(channel_id))
        return;
    const size_t bytes = raw_size(format, stride_w, stride_h);
    if (bytes == 0)
        return;
    const uint64_t now = now_ms();
    const uint64_t period = 1000 / static_cast<uint64_t>(clamp_fps(fps));

    RawFrame raw;
    pthread_mutex_lock(&g_mtx);
    ChannelRing &ring = g_rings[channel_id];
    if (ring.last_capture_ms && now - ring.last_capture_ms < period)
    {
        pthread_mutex_unlock(&g_mtx);
        return;
    }
    ring.last_capture_ms = now;
    if (!ensure_started_locked())
    {
        pthread_mutex_unlock(&g_mtx);
        return;
    }
    std::queue<RawFrame> &channel_queue = g_raw_queues[channel_id];
    if (channel_queue.size() >= CHANNEL_QUEUE_CAPACITY)
    {
        raw = std::move(channel_queue.front());
        channel_queue.pop();
    }
    pthread_mutex_unlock(&g_mtx);

    raw.channel_id = channel_id;
    raw.format = format;
    raw.width = width;
    raw.height = height;
    raw.stride_w = stride_w;
    raw.stride_h = stride_h;
    raw.timestamp_ms = now;
    raw.pre_sec = clamp_sec(pre_sec);
    raw.overlay_mode = overlay_mode;
    raw.bytes.resize(bytes);
    memcpy(raw.bytes.data(), data, bytes);
    raw.rois.clear();
    raw.results.clear();
    raw.commands.clear();
    raw.input_w = raw.input_h = 0;
    raw.disp_fps = 0.0f;
    raw.result_ts_ms = 0;
    raw.swap_rb = false;

    if (overlay_mode != EVENT_VIDEO_OVERLAY_NONE && g_pCtrl)
    {
        auto runtime = app_ctrl_get_runtime_snapshot();
        const std::vector<RoiZone> *runtime_rois = app_ctrl_runtime_channel_rois(runtime, channel_id);
        const ChannelConfig *channel = app_ctrl_runtime_channel_config(runtime, channel_id);
        if (runtime_rois)
            raw.rois = *runtime_rois;

        /* 在源帧入队时原子复制同一时刻的检测结果和逻辑绘制指令。 */
        pthread_mutex_lock(&g_pCtrl->chn_mtx[channel_id]);
        const ChannelState &state = g_pCtrl->channels_state[channel_id];
        raw.results = state.last_results;
        raw.commands = state.draw_cmds;
        raw.disp_fps = state.disp_fps;
        raw.result_ts_ms = state.last_result_ts_ms;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[channel_id]);

        raw.input_w = g_pCtrl->inputW;
        raw.input_h = g_pCtrl->inputH;
        if (channel)
            raw.swap_rb = channel->swap_rb;
    }

    pthread_mutex_lock(&g_mtx);
    if (!g_running)
    {
        pthread_mutex_unlock(&g_mtx);
        return;
    }
    g_raw_queues[channel_id].push(std::move(raw));
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mtx);
}

int event_video_recorder_trigger(const EventVideoRequest &input)
{
    if (!app_ctrl_has_channel(input.channel_id) || input.event_id.empty())
        return 0;
    EventVideoRequest request = input;
    request.fps = clamp_fps(request.fps);
    request.pre_sec = clamp_sec(request.pre_sec);
    request.post_sec = clamp_sec(request.post_sec);
    const uint64_t now = now_ms();
    const std::string key = key_for(request.channel_id, request.event_type);

    pthread_mutex_lock(&g_mtx);
    if (!ensure_started_locked())
    {
        pthread_mutex_unlock(&g_mtx);
        return 0;
    }
    auto latest = g_latest_by_key.find(key);
    if (latest != g_latest_by_key.end())
    {
        auto existing = g_active.find(latest->second);
        if (existing != g_active.end() && existing->second.request.event_id == request.event_id)
        {
            existing->second.end_ms = now + static_cast<uint64_t>(request.post_sec * 1000.0f);
            pthread_mutex_unlock(&g_mtx);
            return 1;
        }
        if (existing == g_active.end())
            g_latest_by_key.erase(latest);
    }
    VideoEvent event;
    event.request = request;
    event.trigger_ms = now;
    event.end_ms = now + static_cast<uint64_t>(request.post_sec * 1000.0f);
    event.start_ms = now > static_cast<uint64_t>(request.pre_sec * 1000.0f)
                         ? now - static_cast<uint64_t>(request.pre_sec * 1000.0f)
                         : 0;
    for (const auto &sample : g_rings[request.channel_id].frames)
        if (sample.timestamp_ms >= event.start_ms && sample.timestamp_ms <= now)
            event.frames.push_back(sample);
    g_active[request.event_id] = std::move(event);
    g_latest_by_key[key] = request.event_id;
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mtx);
    return 1;
}

void event_video_recorder_extend(int channel_id, const std::string &event_type)
{
    pthread_mutex_lock(&g_mtx);
    auto latest = g_latest_by_key.find(key_for(channel_id, event_type));
    if (latest != g_latest_by_key.end())
    {
        auto found = g_active.find(latest->second);
        if (found != g_active.end())
            found->second.end_ms = now_ms() + static_cast<uint64_t>(found->second.request.post_sec * 1000.0f);
    }
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mtx);
}

void event_video_recorder_channel_offline(int channel_id)
{
    if (channel_id < 0 || channel_id >= MAX_CHANNEL_NUM)
        return;
    const uint64_t cutoff_ms = now_ms();
    pthread_mutex_lock(&g_mtx);
    for (auto &entry : g_active)
    {
        VideoEvent &event = entry.second;
        if (event.request.channel_id == channel_id && event.end_ms > cutoff_ms)
            event.end_ms = cutoff_ms;
    }
    /* 队列中断流前已经复制的帧仍会被处理；expire_locked 会等这些帧处理完再封口。 */
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mtx);
}

void event_video_recorder_deinit(void)
{
    pthread_mutex_lock(&g_mtx);
    if (!g_started)
    {
        pthread_mutex_unlock(&g_mtx);
        return;
    }
    g_running = false;
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mtx);
    for (size_t i = 0; i < g_worker_count; ++i)
        pthread_join(g_worker_tids[i], nullptr);
    pthread_mutex_lock(&g_mtx);
    for (auto &ring : g_rings)
    {
        ring.frames.clear();
        ring.last_capture_ms = 0;
    }
    for (int channel_id = 0; channel_id < MAX_CHANNEL_NUM; ++channel_id)
    {
        while (!g_raw_queues[channel_id].empty())
            g_raw_queues[channel_id].pop();
        g_channel_busy[channel_id] = false;
    }
    while (!g_video_jobs.empty())
        g_video_jobs.pop();
    g_active.clear();
    g_latest_by_key.clear();
    g_worker_count = 0;
    g_active_encoders = 0;
    g_next_channel = 0;
    g_started = false;
    pthread_mutex_unlock(&g_mtx);
}
