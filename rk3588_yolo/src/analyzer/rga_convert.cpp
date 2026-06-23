/**
 * @file rga_convert.cpp
 * @brief RGA 硬件图像转换 + YOLO 输入帧准备
 *
 * 职责:
 *   - rga_convert_resize / rga_import_src_fd / rga_convert_resize_handle
 *       → RGA3 硬件格式转换与缩放（NV12→BGR / FD→虚拟地址）
 *   - convertToYoloInput
 *       → 解码帧→模型输入 640×640 BGR（RGA 优先，软件回退）
 *   - rgaFmt
 *       → 格式字符串 → RK_FORMAT_* 枚举
 *
 * ⚠ RGA 硬性约束（必须遵守）:
 *   opt.core 只能是 IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1。
 *   使用 RGA2 或第三核心会导致硬件崩溃（只能断电恢复）。
 *   此文件的 RGA 调用段不得修改 core 参数。
 */

#include "../core/app_ctrl.h"
#include "../core/image_utils.h"
#include "frame_pipeline.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <opencv2/opencv.hpp>
#include <rga/im2d.h>

/*======================== RGA 虚拟地址路径 ========================*/

bool rga_convert_resize(int chnId, const RgaImage &src_img, const RgaImage &dst_img)
{
    rga_buffer_t src = wrapbuffer_virtualaddr(src_img.pBuf, src_img.width, src_img.height, src_img.fmt,
                                              src_img.hor_stride, src_img.ver_stride);
    src.fd = -1;

    rga_buffer_t dst = wrapbuffer_virtualaddr(dst_img.pBuf, dst_img.width, dst_img.height, dst_img.fmt,
                                              dst_img.hor_stride, dst_img.ver_stride);
    dst.fd = -1;

    im_rect srect = {0, 0, src_img.width, src_img.height};
    im_rect drect = {0, 0, dst_img.width, dst_img.height};

    im_opt_t opt{};
    /* ⚠ 仅在 RGA3 双核调度，禁止改动 */
    opt.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1;

    rga_buffer_t pat{};
    im_rect prect{};

    IM_STATUS STATUS = improcess(src, dst, pat, srect, drect, prect, 0, nullptr, &opt, 0);
    if (STATUS != IM_STATUS_SUCCESS)
    {
        fprintf(stderr, "[RGA] ch%d improcess failed: %s\n", chnId, imStrError(STATUS));
        return false;
    }
    return true;
}

/*======================== DMA-BUF FD 导入路径 ========================*/

RgaImportedBuffer::~RgaImportedBuffer()
{
    if (handle != 0)
    {
        releasebuffer_handle(handle);
        handle = 0;
    }
}

std::shared_ptr<RgaImportedBuffer> rga_import_src_fd(int fd, int w, int h, int stride_w, int stride_h, int fmt)
{
    if (fd < 0 || w <= 0 || h <= 0 || stride_w <= 0 || stride_h <= 0)
        return nullptr;

    im_handle_param_t param{};
    param.width = static_cast<uint32_t>(stride_w);
    param.height = static_cast<uint32_t>(stride_h);
    param.format = static_cast<uint32_t>(fmt);

    rga_buffer_handle_t h_id = importbuffer_fd(fd, &param);
    if (h_id == 0)
    {
        static std::atomic<int> cnt{0};
        int c = ++cnt;
        if (c <= 20 || (c % 200) == 0)
            fprintf(stderr,
                    "[RGA] importbuffer_fd failed cnt=%d  fd=%d %dx%d stride=%dx%d "
                    "fmt=%d\n",
                    c, fd, w, h, stride_w, stride_h, fmt);
        return nullptr;
    }

    auto p = std::make_shared<RgaImportedBuffer>();
    p->handle = h_id;
    p->width = w;
    p->height = h;
    p->stride_w = stride_w;
    p->stride_h = stride_h;
    p->format = fmt;
    return p;
}

bool rga_convert_resize_handle(int chnId, const RgaImportedBuffer &src, int dst_fd, int dst_w, int dst_h,
                               int dst_stride_w, int dst_stride_h, int dst_fmt, int cached_dst_handle)
{
    if (src.handle == 0 || dst_fd < 0)
        return false;

    rga_buffer_handle_t dst_handle;
    const bool handle_is_cached = (cached_dst_handle != 0);

    if (handle_is_cached)
    {
        dst_handle = static_cast<rga_buffer_handle_t>(cached_dst_handle);
    }
    else
    {
        im_handle_param_t dst_param{};
        dst_param.width = static_cast<uint32_t>(dst_stride_w);
        dst_param.height = static_cast<uint32_t>(dst_stride_h);
        dst_param.format = static_cast<uint32_t>(dst_fmt);
        dst_handle = importbuffer_fd(dst_fd, &dst_param);
        if (dst_handle == 0)
        {
            static std::atomic<int> cnt{0};
            int c = ++cnt;
            if (c <= 20 || (c % 200) == 0)
                fprintf(stderr,
                        "[RGA] importbuffer_fd dst failed cnt=%d  fd=%d %dx%d "
                        "stride=%dx%d\n",
                        c, dst_fd, dst_w, dst_h, dst_stride_w, dst_stride_h);
            return false;
        }
    }

    rga_buffer_t src_buf = wrapbuffer_handle(src.handle, src.width, src.height, src.format, src.stride_w, src.stride_h);
    rga_buffer_t dst_buf = wrapbuffer_handle(dst_handle, dst_w, dst_h, dst_fmt, dst_stride_w, dst_stride_h);

    im_rect srect = {0, 0, src.width, src.height};
    im_rect drect = {0, 0, dst_w, dst_h};

    im_opt_t opt{};
    opt.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1; /* ⚠ 禁改 */

    rga_buffer_t pat{};
    im_rect prect{};

    IM_STATUS STATUS = improcess(src_buf, dst_buf, pat, srect, drect, prect, 0, nullptr, &opt, 0);

    if (!handle_is_cached)
        releasebuffer_handle(dst_handle);

    if (STATUS != IM_STATUS_SUCCESS)
    {
        static std::atomic<int> cnt{0};
        int c = ++cnt;
        if (c <= 20 || (c % 200) == 0)
            fprintf(stderr, "[RGA] ch%d improcess handle failed cnt=%d: %s\n", chnId, c, imStrError(STATUS));
        return false;
    }
    return true;
}

/*======================== YOLO 输入转换（RGA
 * 优先，软件回退）========================*/

bool convertToYoloInput(int chnId, void *pSrcData, int src_fd, int srcW, int srcH, int srcStrH, int srcStrV, int srcFmt,
                        cv::Mat &out)
{
    if (srcW <= 0 || srcH <= 0 || srcStrH <= 0 || srcStrV <= 0 || (!pSrcData && src_fd < 0))
    {
        static std::atomic<int> cnt{0};
        int c = ++cnt;
        if (c <= 20 || (c % 200) == 0)
            fprintf(stderr, "[yolo_in] skip invalid frame cnt=%d  src=%dx%d stride=%dx%d fd=%d\n", c, srcW, srcH,
                    srcStrH, srcStrV, src_fd);
        return false;
    }

    if (srcFmt == RK_FORMAT_YCbCr_420_SP || srcFmt == RK_FORMAT_YCrCb_420_SP || srcFmt == RK_FORMAT_BGR_888 ||
        srcFmt == RK_FORMAT_RGB_888)
    {
        out.create(g_pCtrl->inputH, g_pCtrl->inputW, CV_8UC3);

        RgaImage dst_img;
        dst_img.fmt = RK_FORMAT_BGR_888;
        dst_img.width = g_pCtrl->inputW;
        dst_img.height = g_pCtrl->inputH;
        dst_img.hor_stride = g_pCtrl->inputW;
        dst_img.ver_stride = g_pCtrl->inputH;
        dst_img.rotation = 0;
        dst_img.pBuf = out.data;

        bool rga_ok = false;

        if (src_fd >= 0)
        {
            rga_buffer_t src = wrapbuffer_fd(src_fd, srcW, srcH, srcFmt, srcStrH, srcStrV);
            rga_buffer_t dst = wrapbuffer_virtualaddr(dst_img.pBuf, dst_img.width, dst_img.height, dst_img.fmt,
                                                      dst_img.hor_stride, dst_img.ver_stride);
            dst.fd = -1;

            im_rect srect = {0, 0, srcW, srcH};
            im_rect drect = {0, 0, dst_img.width, dst_img.height};

            im_opt_t opt{};
            opt.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1; /* ⚠ 禁改 */

            rga_buffer_t pat{};
            im_rect prect{};
            rga_ok = (improcess(src, dst, pat, srect, drect, prect, 0, nullptr, &opt, 0) == IM_STATUS_SUCCESS);
        }
        else
        {
            RgaImage src_img;
            src_img.fmt = static_cast<RgaSURF_FORMAT>(srcFmt);
            src_img.width = srcW;
            src_img.height = srcH;
            src_img.hor_stride = srcStrH;
            src_img.ver_stride = srcStrV;
            src_img.rotation = 0;
            src_img.pBuf = pSrcData;
            rga_ok = rga_convert_resize(chnId, src_img, dst_img);
        }

        if (rga_ok)
            return true;

        static std::atomic<int> cnt{0};
        int c = ++cnt;
        if (c <= 20 || (c % 200) == 0)
            fprintf(stderr, "[yolo_in] RGA fail cnt=%d  src=%dx%d stride=%dx%d fmt=%d\n", c, srcW, srcH, srcStrH,
                    srcStrV, srcFmt);
    }

    /* 软件回退：raw → BGR → resize */
    out.release();
    thread_local cv::Mat bgr;
    if (raw_to_bgr_mat(pSrcData, srcW, srcH, srcStrH, srcStrV, srcFmt, bgr))
    {
        cv::resize(bgr, out, cv::Size(g_pCtrl->inputW, g_pCtrl->inputH));
        return true;
    }
    return false;
}

/*======================== 格式字符串 → RK_FORMAT ========================*/

int rgaFmt(const char *strFmt)
{
    if (0 == strcmp(strFmt, "NV12"))
        return RK_FORMAT_YCbCr_420_SP;
    if (0 == strcmp(strFmt, "NV21"))
        return RK_FORMAT_YCrCb_420_SP;
    if (0 == strcmp(strFmt, "BGR"))
        return RK_FORMAT_BGR_888;
    if (0 == strcmp(strFmt, "RGB"))
        return RK_FORMAT_RGB_888;
    return RK_FORMAT_UNKNOWN;
}
