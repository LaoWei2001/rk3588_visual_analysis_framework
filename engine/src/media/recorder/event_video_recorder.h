#pragma once

#include <cstdint>
#include <string>

enum EventVideoOverlayMode : uint8_t
{
    EVENT_VIDEO_OVERLAY_NONE = 0,
    EVENT_VIDEO_OVERLAY_CUSTOM = 1,
    EVENT_VIDEO_OVERLAY_ALL = 2,
    /* Same size and render rules as live display, without using framebuffer. */
    EVENT_VIDEO_OVERLAY_DISPLAY = 3
};

struct EventVideoRequest
{
    std::string event_id;
    int channel_id = -1; /* 唯一通道身份：config.channels[].id */
    std::string event_type;
    float pre_sec = 5.0f;
    float post_sec = 5.0f;
    int fps = 5;
    std::string output_path;
};

/* 帧缓存入口：可接收原始解码帧或已渲染显示帧；按 fps 节流，转换/压缩在录像线程完成。 */
void event_video_recorder_push_source_frame(int channel_id, const void *data, int format, int width, int height,
                                            int stride_w, int stride_h, int fps, float pre_sec,
                                            EventVideoOverlayMode overlay_mode);

int event_video_recorder_trigger(const EventVideoRequest &request);
void event_video_recorder_extend(const std::string &event_id);
/* 视频源结束/断流：以当前时刻截断该通道尚未结束的 post 窗口；已进入队列的帧会先处理完。 */
void event_video_recorder_channel_offline(int channel_id);
void event_video_recorder_deinit(void);
