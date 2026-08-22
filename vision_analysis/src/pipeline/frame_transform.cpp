/**
 * @file frame_transform.cpp
 * @brief RGA 硬件图像转换 + YOLO 输入帧准备
 *
 * 职责:
 *   - rga_convert_resize / rga_import_src_fd / rga_convert_resize_handle
 *       → RGA3 硬件格式转换与缩放（NV12→BGR / FD→虚拟地址）
 *   - LazyVideoFrame
 *       → Logic 按需取得模型尺寸或原始尺寸 BGR（RGA 优先，软件回退）
 *   - rga_format_from_name
 *       → 格式字符串 → RK_FORMAT_* 枚举
 *
 * ⚠ RGA 硬性约束（必须遵守）:
 *   opt.core 只能是 IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1。
 *   使用 RGA2 或第三核心会导致硬件崩溃（只能断电恢复）。
 *   此文件的 RGA 调用段不得修改 core 参数。
 */

#include "pipeline/image_convert.h"
#include "frame_transform.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <utility>
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
            fprintf(stderr, "[RGA] importbuffer_fd failed cnt=%d  fd=%d %dx%d stride=%dx%d fmt=%d\n", c, fd, w, h,
                    stride_w, stride_h, fmt);
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
                fprintf(stderr, "[RGA] importbuffer_fd dst failed cnt=%d  fd=%d %dx%d stride=%dx%d\n", c, dst_fd, dst_w,
                        dst_h, dst_stride_w, dst_stride_h);
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

bool rga_convert_resize_handle_to_bgr(int chnId, const RgaImportedBuffer &src, int dst_w, int dst_h, cv::Mat &out)
{
    if (src.handle == 0 || dst_w <= 0 || dst_h <= 0)
        return false;

    out.create(dst_h, dst_w, CV_8UC3);
    rga_buffer_t src_buf =
        wrapbuffer_handle(src.handle, src.width, src.height, src.format, src.stride_w, src.stride_h);

    /* librga 1.9+ 不允许同一次任务混用 handle 和裸地址。源帧来自 DMA-BUF，
     * 已经是 handle；因此把 cv::Mat 的目标内存也临时导入为 handle。目标 handle
     * 只在本次同步 improcess 调用期间有效，调用结束后立即释放。 */
    im_handle_param_t dst_param{};
    dst_param.width = static_cast<uint32_t>(dst_w);
    dst_param.height = static_cast<uint32_t>(dst_h);
    dst_param.format = static_cast<uint32_t>(RK_FORMAT_BGR_888);
    const rga_buffer_handle_t dst_handle = importbuffer_virtualaddr(out.data, &dst_param);
    if (dst_handle == 0)
    {
        out.release();
        static std::atomic<int> import_cnt{0};
        const int c = ++import_cnt;
        if (c <= 20 || (c % 200) == 0)
            fprintf(stderr, "[RGA] ch%d import BGR destination failed cnt=%d  %dx%d\n", chnId, c, dst_w, dst_h);
        return false;
    }
    rga_buffer_t dst_buf = wrapbuffer_handle(dst_handle, dst_w, dst_h, RK_FORMAT_BGR_888, dst_w, dst_h);

    im_rect srect = {0, 0, src.width, src.height};
    im_rect drect = {0, 0, dst_w, dst_h};
    im_opt_t opt{};
    opt.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1; /* ⚠ 禁改 */
    rga_buffer_t pat{};
    im_rect prect{};
    const IM_STATUS status = improcess(src_buf, dst_buf, pat, srect, drect, prect, 0, nullptr, &opt, 0);
    releasebuffer_handle(dst_handle);
    if (status == IM_STATUS_SUCCESS)
        return true;

    out.release();
    static std::atomic<int> cnt{0};
    const int c = ++cnt;
    if (c <= 20 || (c % 200) == 0)
        fprintf(stderr, "[RGA] ch%d handle-to-BGR failed cnt=%d: %s\n", chnId, c, imStrError(status));
    return false;
}

LazyVideoFrame::LazyVideoFrame(int channel_id, std::shared_ptr<RgaImportedBuffer> source, int source_width,
                               int source_height, int source_stride_w, int source_stride_h, int source_format,
                               int model_width, int model_height, const void *borrowed_data)
    : channel_id_(channel_id), source_(std::move(source)), source_width_(source_width), source_height_(source_height),
      source_stride_w_(source_stride_w), source_stride_h_(source_stride_h), source_format_(source_format),
      model_width_(model_width), model_height_(model_height), borrowed_data_(borrowed_data)
{
}

bool LazyVideoFrame::materialize_borrowed(int dst_width, int dst_height, cv::Mat &out)
{
    if (!borrowed_data_ || source_width_ <= 0 || source_height_ <= 0 || source_stride_w_ <= 0 ||
        source_stride_h_ <= 0 || dst_width <= 0 || dst_height <= 0)
        return false;

    if (dst_width == source_width_ && dst_height == source_height_)
        return convert_raw_to_bgr(borrowed_data_, source_width_, source_height_, source_stride_w_, source_stride_h_,
                              source_format_, out);

    out.create(dst_height, dst_width, CV_8UC3);
    RgaImage src_img;
    src_img.fmt = static_cast<RgaSURF_FORMAT>(source_format_);
    src_img.width = source_width_;
    src_img.height = source_height_;
    src_img.hor_stride = source_stride_w_;
    src_img.ver_stride = source_stride_h_;
    src_img.rotation = 0;
    src_img.pBuf = const_cast<void *>(borrowed_data_);
    RgaImage dst_img;
    dst_img.fmt = RK_FORMAT_BGR_888;
    dst_img.width = dst_width;
    dst_img.height = dst_height;
    dst_img.hor_stride = dst_width;
    dst_img.ver_stride = dst_height;
    dst_img.rotation = 0;
    dst_img.pBuf = out.data;
    if (rga_convert_resize(channel_id_, src_img, dst_img))
        return true;

    cv::Mat source_bgr;
    if (!convert_raw_to_bgr(borrowed_data_, source_width_, source_height_, source_stride_w_, source_stride_h_,
                        source_format_, source_bgr))
    {
        out.release();
        return false;
    }
    cv::resize(source_bgr, out, cv::Size(dst_width, dst_height));
    return !out.empty();
}

const cv::Mat *LazyVideoFrame::model_frame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!model_attempted_)
    {
        model_attempted_ = true;
        const bool converted = source_ &&
                               rga_convert_resize_handle_to_bgr(channel_id_, *source_, model_width_, model_height_,
                                                                model_bgr_);
        if (!converted)
            materialize_borrowed(model_width_, model_height_, model_bgr_);
    }
    return model_bgr_.empty() ? nullptr : &model_bgr_;
}

const cv::Mat *LazyVideoFrame::source_frame()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!source_attempted_)
    {
        source_attempted_ = true;
        const bool converted = source_ &&
                               rga_convert_resize_handle_to_bgr(channel_id_, *source_, source_width_, source_height_,
                                                                source_bgr_);
        if (!converted)
            materialize_borrowed(source_width_, source_height_, source_bgr_);
    }
    return source_bgr_.empty() ? nullptr : &source_bgr_;
}

void LazyVideoFrame::clear_borrowed_source()
{
    std::lock_guard<std::mutex> lock(mutex_);
    borrowed_data_ = nullptr;
}

bool LazyVideoFrame::available() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return source_ || borrowed_data_ || !model_bgr_.empty() || !source_bgr_.empty();
}

/*======================== 格式字符串 → RK_FORMAT ========================*/

int rga_format_from_name(const char *strFmt)
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
