/**
 * @file display_render.cpp
 * @brief 显示 tile 布局计算 + 帧渲染提交
 *
 * 职责:
 *   - tile_x / tile_y / tile_width / tile_height / calcBufMapOffset
 *       → 多路画面在 framebuffer 上的位置计算
 *   - commitImgtoDispBufMap
 *       → NV12/BGR → staging (heap BGR, 16 对齐) → overlay → RGB → framebuffer
 *
 * 设计要点（勿改）:
 *   ① staging 为 heap cv::Mat，宽度向上对齐到 16 像素（满足 RGA RGB888 约束）。
 *   ② RGA 写 staging，overlay 和拷贝只用 staging_view（实际可见列），
 *      不触碰对齐填充区，保证 display 不显示杂色边框。
 *   ③ display_lock / display_unlock 保护 copyTo 到 front_roi 的操作（避免 GTK 撕裂）。
 *   ④ RGA 段（rga_convert_resize 调用）不在 display_lock 内，避免把锁争用误报为 RGA 错误。
 */

#include "frame_pipeline.h"
#include "../core/app_ctrl.h"
#include "../core/image_utils.h"
#include "../player/display.h"
#include "../logic/channel_logic.h"   /* DrawCommand, RenderParams */

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <pthread.h>
#include <opencv2/opencv.hpp>

/*======================== tile 布局计算 ========================*/

int tile_x(int chnId)
{
    int cols = app_ctrl_get_tile_cols();
    return ((chnId % cols) * (app_ctrl_get_disp_width() / cols)) & ~3;
}

int tile_y(int chnId)
{
    int cols = app_ctrl_get_tile_cols();
    int rows = app_ctrl_get_tile_rows();
    return ((chnId / cols) * (app_ctrl_get_disp_height() / rows)) & ~1;
}

int tile_width(int chnId)
{
    int cols   = app_ctrl_get_tile_cols();
    int grid_w = app_ctrl_get_disp_width() / cols;
    int x_left  = ((chnId % cols) * grid_w) & ~3;
    int x_right = (((chnId % cols) + 1) * grid_w) & ~3;
    if ((chnId % cols) == cols - 1)
        x_right = app_ctrl_get_disp_width() & ~3;
    return std::max(2, x_right - x_left);
}

int tile_height(int chnId)
{
    int cols   = app_ctrl_get_tile_cols();
    int rows   = app_ctrl_get_tile_rows();
    int grid_h = app_ctrl_get_disp_height() / rows;
    int y_top    = ((chnId / cols) * grid_h) & ~1;
    int y_bottom = (((chnId / cols) + 1) * grid_h) & ~1;
    if ((chnId / cols) == rows - 1)
        y_bottom = app_ctrl_get_disp_height() & ~1;
    return std::max(2, y_bottom - y_top);
}

uint64_t calcBufMapOffset(int chnId, int bytesPerPixel)
{
    return (uint64_t)bytesPerPixel *
           (uint64_t)(tile_y(chnId) * app_ctrl_get_disp_width() + tile_x(chnId));
}

/*======================== 帧渲染提交 ========================*/
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
void commitImgtoDispBufMap(int chnId, const void *pSrcData, int srcFmt,
                            int srcWidth, int srcHeight,
                            int srcHStride, int srcVStride)
{
    static constexpr int SCREEN_BPP = 3;
    char *pFront = *g_pCtrl->pDispBuffer;
    if (!pFront) return;

    if (!pSrcData || srcWidth <= 0 || srcHeight <= 0 ||
        srcHStride <= 0 || srcVStride <= 0)
    {
        static std::atomic<int> cnt{0};
        int c = ++cnt;
        if (c <= 20 || (c % 200) == 0)
            fprintf(stderr,
                    "[commit] skip invalid frame ch=%d cnt=%d  src=%dx%d stride=%dx%d pBuf=%p\n",
                    chnId, c, srcWidth, srcHeight, srcHStride, srcVStride, pSrcData);
        return;
    }

    const int tile_w = tile_width(chnId);
    const int tile_h = tile_height(chnId);
    const int disp_w = app_ctrl_get_disp_width();

    /* RGA RGB888 要求目标 stride 为 16 像素的倍数，向上对齐 */
    const int tile_aligned_w = (tile_w + 15) & ~15;

    auto &cs = g_pCtrl->channels_state[chnId];
    if (cs.tile_staging.empty() ||
        cs.tile_staging.cols != tile_aligned_w ||
        cs.tile_staging.rows != tile_h)
    {
        cs.tile_staging.create(tile_h, tile_aligned_w, CV_8UC3);
    }
    cv::Mat &staging      = cs.tile_staging;
    cv::Mat  staging_view = staging(cv::Rect(0, 0, tile_w, tile_h));

    bool rga_ok = false;
    if (srcFmt == RK_FORMAT_YCbCr_420_SP || srcFmt == RK_FORMAT_YCrCb_420_SP ||
        srcFmt == RK_FORMAT_BGR_888       || srcFmt == RK_FORMAT_RGB_888)
    {
        RgaImage src_img;
        src_img.fmt        = static_cast<RgaSURF_FORMAT>(srcFmt);
        src_img.width      = srcWidth;
        src_img.height     = srcHeight;
        src_img.hor_stride = srcHStride;
        src_img.ver_stride = srcVStride;
        src_img.rotation   = 0;
        src_img.pBuf       = const_cast<void *>(pSrcData);

        RgaImage dst_img;
        dst_img.fmt        = RK_FORMAT_BGR_888;
        dst_img.width      = tile_w;
        dst_img.height     = tile_h;
        dst_img.hor_stride = tile_aligned_w;
        dst_img.ver_stride = tile_h;
        dst_img.rotation   = 0;
        dst_img.pBuf       = staging.data;

        rga_ok = rga_convert_resize(chnId, src_img, dst_img);

        if (!rga_ok)
        {
            static std::atomic<int> cnt{0};
            int c = ++cnt;
            if (c <= 20 || (c % 200) == 0)
                fprintf(stderr,
                        "[commit] RGA fail ch=%d cnt=%d  src=%dx%d stride=%dx%d fmt=%d"
                        "  dst=%dx%d (aligned_stride=%d)\n",
                        chnId, c, srcWidth, srcHeight, srcHStride, srcVStride, srcFmt,
                        tile_w, tile_h, tile_aligned_w);
        }
    }

    if (!rga_ok)
    {
        thread_local cv::Mat bgr, tile_bgr;
        if (raw_to_bgr_mat(const_cast<void *>(pSrcData),
                           srcWidth, srcHeight, srcHStride, srcVStride, srcFmt, bgr))
        {
            cv::resize(bgr, tile_bgr, cv::Size(tile_w, tile_h));
            tile_bgr.copyTo(staging_view);
        }
        else
        {
            return; /* 软件回退也失败，放弃本帧 */
        }
    }

    /* 取 results + draw_cmds（一把 chn_mtx 原子读，消除竞态）*/
    float                    infer_fps = algorithm_get_infer_fps(chnId);
    std::vector<AlgoResult>  disp_results;
    std::vector<DrawCommand> disp_draw_cmds;
    int64_t result_age_ms = 0;
    cv::Mat  logic_disp;                 /* logic 经 display_canvas() 自绘的显示底图(空=无) */
    bool     logic_disp_fresh = false;
    {
        pthread_mutex_lock(&g_pCtrl->chn_mtx[chnId]);
        disp_results   = g_pCtrl->channels_state[chnId].last_results;
        disp_draw_cmds = g_pCtrl->channels_state[chnId].draw_cmds;
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        result_age_ms = static_cast<int64_t>(now_ms)
                        - static_cast<int64_t>(
                              g_pCtrl->channels_state[chnId].last_result_ts_ms);
        logic_disp = g_pCtrl->channels_state[chnId].logic_display_frame;   /* 浅拷贝(引用计数), O(1) */
        logic_disp_fresh = !logic_disp.empty()
            && (static_cast<int64_t>(now_ms)
                - static_cast<int64_t>(g_pCtrl->channels_state[chnId].logic_display_ts_ms)) < 1000;
        pthread_mutex_unlock(&g_pCtrl->chn_mtx[chnId]);
    }
    result_age_ms = std::max(int64_t(0), std::min(result_age_ms, int64_t(200)));

    RenderParams rp;
    rp.chnId         = chnId;
    rp.inputW        = g_pCtrl->inputW;
    rp.inputH        = g_pCtrl->inputH;
    rp.disp_fps      = cs.disp_fps;
    rp.infer_fps     = infer_fps;
    rp.result_age_ms = result_age_ms;
    rp.roi_zones     = &cs.roi_zones;   /* 多 ROI: 全部区域(模型坐标系), render_overlays 逐个绘制 */
    rp.results       = &disp_results;
    rp.draw_cmds     = &disp_draw_cmds;
    /* logic 用 display_canvas() 拦截了整帧 → 用它(640×640 BGR)当显示底图(缩放到 tile)，覆盖实时帧。
     * 仅在新鲜(1s内)生效；陈旧则保留实时帧，防 logic 停更后画面冻结。draw_cmds/检测框照旧叠加其上。*/
    if (logic_disp_fresh)
        cv::resize(logic_disp, staging_view, staging_view.size());
    render_overlays(staging_view, rp);

    /* BGR → RGB（GTK 期望 RGB），再 copyTo front tile（持 display_lock 防撕裂）。
     * 本通道 swap_rb=1 时故意跳过这步：GTK 把 BGR 当 RGB 解析 → 屏幕上 R/B 互换显示
     * （仅影响显示；推理/上报仍用正常 BGR，不受影响）。 */
    if (!g_pCtrl->config.channels[chnId].swap_rb)
        cv::cvtColor(staging_view, staging_view, cv::COLOR_BGR2RGB);
    uint64_t dstOffset = calcBufMapOffset(chnId, SCREEN_BPP);
    cv::Mat  front_roi(tile_h, tile_w, CV_8UC3, pFront + dstOffset, disp_w * SCREEN_BPP);
    display_lock();
    staging_view.copyTo(front_roi);
    display_unlock();

    /* 显示 FPS 统计（display_worker 独占 cs，无需锁）*/
    auto now_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    cs.fps_counter++;
    int64_t elapsed = static_cast<int64_t>(now_ts) -
                      static_cast<int64_t>(cs.last_fps_ts_ms);
    if (elapsed >= 1000)
    {
        cs.disp_fps       = (1000.0f * cs.fps_counter) / static_cast<float>(elapsed);
        cs.fps_counter    = 0;
        cs.last_fps_ts_ms = static_cast<uint64_t>(now_ts);
    }
}
