/**
 * @file display_render.cpp
 * @brief 显示 tile 布局计算 + 帧渲染提交
 *
 * 职责:
 *   - tile_x / tile_y / tile_width / tile_height / display_buffer_offset
 *       → 多路画面在 framebuffer 上的位置计算
 *   - display_commit_frame
 *       → NV12/BGR → staging (heap BGR, 16 对齐) → overlay → RGB → framebuffer
 *
 * 设计要点（勿改）:
 *   ① staging 为 heap cv::Mat，宽度向上对齐到 16 像素（满足 RGA RGB888 约束）。
 *   ② RGA 写 staging，overlay 和拷贝只用 staging_view（实际可见列），
 *      不触碰对齐填充区，保证 display 不显示杂色边框。
 *   ③ display_lock / display_unlock 保护 copyTo 到 front_roi 的操作（避免 GTK 撕裂）。
 *   ④ RGA 段（rga_convert_resize 调用）不在 display_lock 内，避免把锁争用误报为 RGA 错误。
 */

#include "display.h"
#include "display_pipeline.h"
#include "inference/inference_engine.h"
#include "pipeline/frame_transform.h"
#include "pipeline/image_convert.h"
#include "runtime/app_ctrl.h"
#include "runtime/pause_ctrl.h"
#include <channel.h> /* DrawCommand, RenderParams */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <opencv2/opencv.hpp>
#include <pthread.h>

/*======================== tile 布局计算 ========================*/

int tile_x(int chnId)
{
    const int display_order = app_ctrl_get_channel_display_order(chnId);
    if (display_order < 0)
        return 0;
    int cols = app_ctrl_get_tile_cols();
    return ((display_order % cols) * (app_ctrl_get_disp_width() / cols)) & ~3;
}

int tile_y(int chnId)
{
    const int display_order = app_ctrl_get_channel_display_order(chnId);
    if (display_order < 0)
        return 0;
    int cols = app_ctrl_get_tile_cols();
    int rows = app_ctrl_get_tile_rows();
    return ((display_order / cols) * (app_ctrl_get_disp_height() / rows)) & ~1;
}

int tile_width(int chnId)
{
    const int display_order = app_ctrl_get_channel_display_order(chnId);
    if (display_order < 0)
        return 0;
    int cols = app_ctrl_get_tile_cols();
    int grid_w = app_ctrl_get_disp_width() / cols;
    int x_left = ((display_order % cols) * grid_w) & ~3;
    int x_right = (((display_order % cols) + 1) * grid_w) & ~3;
    if ((display_order % cols) == cols - 1)
        x_right = app_ctrl_get_disp_width() & ~3;
    return std::max(2, x_right - x_left);
}

int tile_height(int chnId)
{
    const int display_order = app_ctrl_get_channel_display_order(chnId);
    if (display_order < 0)
        return 0;
    int cols = app_ctrl_get_tile_cols();
    int rows = app_ctrl_get_tile_rows();
    int grid_h = app_ctrl_get_disp_height() / rows;
    int y_top = ((display_order / cols) * grid_h) & ~1;
    int y_bottom = (((display_order / cols) + 1) * grid_h) & ~1;
    if ((display_order / cols) == rows - 1)
        y_bottom = app_ctrl_get_disp_height() & ~1;
    return std::max(2, y_bottom - y_top);
}

uint64_t display_buffer_offset(int chnId, int bytesPerPixel)
{
    return (uint64_t)bytesPerPixel * (uint64_t)(tile_y(chnId) * app_ctrl_get_disp_width() + tile_x(chnId));
}

/*======================== 帧渲染提交 ========================*/
/* 必须由该通道 display_worker 调用，图像生命周期与首帧合成保持一致。 */
static void write_channel_view(int chnId, cv::Mat &view)
{
    render_channel_view(view, chnId, 0, &g_pCtrl->channels_state[chnId].performance_overlay);
    auto runtime = app_ctrl_get_runtime_snapshot();
    const ChannelConfig *channel = app_ctrl_runtime_channel_config(runtime, chnId);
    if (!channel || !channel->swap_rb)
        cv::cvtColor(view, view, cv::COLOR_BGR2RGB);
    constexpr int screen_bpp = 3;
    char *front = *g_pCtrl->pDispBuffer;
    cv::Mat front_roi(view.rows, view.cols, CV_8UC3, front + display_buffer_offset(chnId, screen_bpp),
                      app_ctrl_get_disp_width() * screen_bpp);
    display_lock();
    view.copyTo(front_roi);
    display_unlock();
}

/**
 * @brief 将单通道帧渲染到 GTK 显示缓冲区对应的 tile 区域。
 *
 * 管线:
 *   src (NV12/BGR, pSrcData)
 *     → [rga_convert_resize] → staging (heap BGR, 16px-aligned stride)
 *     → [render_overlays]    → staging_view（overlay：框、文字、ROI 等）
 *     → [cvtColor BGR→RGB]   → staging_view（GTK 期望 RGB）
 *     → [copyTo front_roi]   → framebuffer tile（持 display_lock）
 *
 * 零尺寸帧保护：RTSP 重连期间上游可能推入无效帧，在入口早退避免 RGA 崩溃。
 */
void display_commit_frame(int chnId, const void *pSrcData, int srcFmt, int srcWidth, int srcHeight, int srcHStride,
                          int srcVStride)
{
    char *pFront = *g_pCtrl->pDispBuffer;
    if (!pFront)
        return;

    if (!pSrcData || srcWidth <= 0 || srcHeight <= 0 || srcHStride <= 0 || srcVStride <= 0)
    {
        static std::atomic<int> cnt{0};
        int c = ++cnt;
        if (c <= 20 || (c % 200) == 0)
            fprintf(stderr, "[commit] skip invalid frame ch=%d cnt=%d  src=%dx%d stride=%dx%d pBuf=%p\n", chnId, c,
                    srcWidth, srcHeight, srcHStride, srcVStride, pSrcData);
        return;
    }

    const int tile_w = tile_width(chnId);
    const int tile_h = tile_height(chnId);

    /* RGA RGB888 要求目标 stride 为 16 像素的倍数，向上对齐 */
    const int tile_aligned_w = (tile_w + 15) & ~15;

    auto &cs = g_pCtrl->channels_state[chnId];
    if (cs.tile_staging.empty() || cs.tile_staging.cols != tile_aligned_w || cs.tile_staging.rows != tile_h)
    {
        cs.tile_staging.create(tile_h, tile_aligned_w, CV_8UC3);
    }
    cv::Mat &staging = cs.tile_staging;
    cv::Mat staging_view = staging(cv::Rect(0, 0, tile_w, tile_h));

    bool rga_ok = false;
    if (srcFmt == RK_FORMAT_YCbCr_420_SP || srcFmt == RK_FORMAT_YCrCb_420_SP || srcFmt == RK_FORMAT_BGR_888 ||
        srcFmt == RK_FORMAT_RGB_888)
    {
        RgaImage src_img;
        src_img.fmt = static_cast<RgaSURF_FORMAT>(srcFmt);
        src_img.width = srcWidth;
        src_img.height = srcHeight;
        src_img.hor_stride = srcHStride;
        src_img.ver_stride = srcVStride;
        src_img.rotation = 0;
        src_img.pBuf = const_cast<void *>(pSrcData);

        RgaImage dst_img;
        dst_img.fmt = RK_FORMAT_BGR_888;
        dst_img.width = tile_w;
        dst_img.height = tile_h;
        dst_img.hor_stride = tile_aligned_w;
        dst_img.ver_stride = tile_h;
        dst_img.rotation = 0;
        dst_img.pBuf = staging.data;

        rga_ok = rga_convert_resize(chnId, src_img, dst_img);

        if (!rga_ok)
        {
            static std::atomic<int> cnt{0};
            int c = ++cnt;
            if (c <= 20 || (c % 200) == 0)
                fprintf(stderr,
                        "[commit] RGA fail ch=%d cnt=%d  src=%dx%d stride=%dx%d fmt=%d"
                        "  dst=%dx%d (aligned_stride=%d)\n",
                        chnId, c, srcWidth, srcHeight, srcHStride, srcVStride, srcFmt, tile_w, tile_h, tile_aligned_w);
        }
    }

    if (!rga_ok)
    {
        thread_local cv::Mat bgr, tile_bgr;
        if (convert_raw_to_bgr(const_cast<void *>(pSrcData), srcWidth, srcHeight, srcHStride, srcVStride, srcFmt, bgr))
        {
            cv::resize(bgr, tile_bgr, cv::Size(tile_w, tile_h));
            tile_bgr.copyTo(staging_view);
        }
        else
        {
            return; /* 软件回退也失败，放弃本帧 */
        }
    }

    write_channel_view(chnId, staging_view);
    cs.preview_rate.tick();
}

void display_refresh_channel_overlays(int chnId)
{
    auto &cs = g_pCtrl->channels_state[chnId];
    if (cs.tile_staging.empty() || !g_pCtrl->pDispBuffer || !*g_pCtrl->pDispBuffer)
        return;
    const int tile_w = tile_width(chnId), tile_h = tile_height(chnId);
    if (cs.tile_staging.cols < tile_w || cs.tile_staging.rows != tile_h)
        return;
    auto &cache = cs.performance_overlay;
    const bool show_perf = app_ctrl_get_performance_display();
    RenderParams params;
    params.chnId = chnId;
    params.disp_fps = app_ctrl_get_disp_fps(chnId);
    params.infer_fps = inference_get_infer_fps(chnId);
    if (pause_ctrl::is_paused())
        params.disp_fps = params.infer_fps = 0.0f;
    if (show_perf == cache.visible &&
        (!show_perf || (params.disp_fps == cache.render_fps && params.infer_fps == cache.infer_fps)))
        return; // 数字没变：不拷贝、不画字、不写 framebuffer。

    cv::Mat view = cs.tile_staging(cv::Rect(0, 0, tile_w, tile_h));
    const cv::Rect bounds = performance_overlay_bounds(view);
    cv::Mat footer = view(bounds);
    const auto runtime = app_ctrl_get_runtime_snapshot();
    const ChannelConfig *channel = app_ctrl_runtime_channel_config(runtime, chnId);
    const bool swap_rb = channel && channel->swap_rb;
    if (cache.background.empty())
    {
        // 停帧时刚打开性能显示：从当前显示帧取得背景，无需预先保存整帧。
        footer.copyTo(cache.background);
        if (!swap_rb)
            cv::cvtColor(cache.background, cache.background, cv::COLOR_RGB2BGR);
    }
    if (cache.background.size() != footer.size())
        return;
    cache.background.copyTo(footer);
    render_performance_overlay(view, params);
    if (!swap_rb)
        cv::cvtColor(footer, footer, cv::COLOR_BGR2RGB);
    constexpr int screen_bpp = 3;
    char *front = *g_pCtrl->pDispBuffer;
    cv::Mat front_roi(tile_h, tile_w, CV_8UC3, front + display_buffer_offset(chnId, screen_bpp),
                      app_ctrl_get_disp_width() * screen_bpp);
    display_lock();
    footer.copyTo(front_roi(bounds));
    display_unlock();
    cache.visible = show_perf;
    cache.render_fps = params.disp_fps;
    cache.infer_fps = params.infer_fps;
    if (!show_perf)
        cache.background.release();
}
