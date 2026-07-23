#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <opencv2/opencv.hpp>

/* RenderParams 定义在 channel_logic.h（DrawCommand / AlgoResult 也在那里）
 * 这里只做前置声明，避免循环 include：
 *   channel_logic.h → display.h → channel_logic.h */
struct RenderParams;

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

// Display buffer lock to avoid tearing when GTK reads while analyzer writes.
void display_lock();
void display_unlock();
bool display_try_lock();

/**
 * @brief 将所有 overlay（ROI、检测框、draw_cmds、FPS 文字）画到 screen_roi 上。
 *
 * screen_roi 必须是 BGR 格式的 cv::Mat（与 OpenCV draw 函数约定一致）。
 * display 路径在写入 front buffer 前会统一做 BGR→RGB 转换。
 *
 * RenderParams 完整定义在 channel_logic.h，调用方 .cpp 须先 include channel_logic.h。
 */
void render_overlays(cv::Mat &screen_roi, const RenderParams &p);

/* Render one channel with the same rules as the live view. The caller owns the
 * BGR image and target size; this function never reads or writes framebuffer. */
void render_channel_view(cv::Mat &bgr, int chn_id, uint64_t frame_timestamp_ms = 0);
