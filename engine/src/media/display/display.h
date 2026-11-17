#pragma once
#include <opencv2/opencv.hpp>
#include <rendering.h>
#include <stdbool.h>
#include <stdint.h>
#include <types.h>

struct ChannelContext;
vision::OverlayOptions channel_render_params(const ChannelContext &ctx);
bool channel_uses_depth_background(const ChannelConfig *channel);

void render_runtime_performance_overlay(cv::Mat &view, const vision::OverlayOptions &params,
                                        vision::OverlayCache *cache = nullptr);

typedef struct
{
    const char *winTitle;
    int x;
    int y;
    int width;
    int height;
} Display_t;

char **dispBufferMap(Display_t *dispDesc);
void dispBufferUnmap(void);
int display(Display_t *dispDesc);

// Display buffer lock to avoid tearing when GTK reads while pipeline writes.
void display_lock();
void display_unlock();
bool display_try_lock();

/**
 * @brief 将所有 overlay（ROI、检测框、draw_cmds、FPS 文字）画到 screen_roi 上。
 *
 * screen_roi 必须是 BGR 格式的 cv::Mat（与 OpenCV draw 函数约定一致）。
 * display 路径在写入 front buffer 前会统一做 BGR→RGB 转换。
 *
 * 运行时适配层应用当前性能显示开关，再调用公共 vision::render_overlays。
 */
void render_runtime_overlays(cv::Mat &screen_roi, const vision::OverlayOptions &p,
                             vision::OverlayCache *performance_cache = nullptr);

/* Render one channel with the same rules as the live view. The caller owns the
 * BGR image and target size; this function never reads or writes framebuffer. */
void render_channel_view(cv::Mat &bgr, int chn_id, uint64_t frame_timestamp_ms = 0,
                         vision::OverlayCache *performance_cache = nullptr);
