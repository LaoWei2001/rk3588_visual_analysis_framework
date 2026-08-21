/**
 * @file frame_pipeline.h
 * @brief 帧处理管线 — RGA 硬件转换 + 显示缓冲区提交
 *
 * 职责:
 * - RGA 硬件加速的图像格式转换与缩放
 * - 显示 tile 布局计算
 * - 显示缓冲区写入 (双缓冲)
 *
 * 实现分布 (本 .h 的声明分散在两个 .cpp 中; 原空壳 frame_pipeline.cpp 已删除):
 *   rga_convert.cpp    — RGA 硬件转换 + YOLO 输入帧准备
 *                        (rga_convert_resize / rga_import_src_fd /
 *                         rga_convert_resize_handle / LazyVideoFrame / rgaFmt)
 *   display_render.cpp — 显示 tile 布局 + framebuffer 提交
 *                        (tile_x / tile_y / tile_width / tile_height /
 *                         calcBufMapOffset / commitImgtoDispBufMap)
 *
 * 注意: RGA 部分代码禁止修改内部逻辑, 盒子容易死机! 硬性约束 (在 rga_convert.cpp 中):
 *   opt.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1
 *   使用 RGA2 或第三核心会硬崩溃, 只能断电恢复。
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <rga/RgaApi.h>
#include <rga/im2d.h>

/*======================== RGA 图像描述 ========================*/
typedef struct
{
    RgaSURF_FORMAT fmt;
    int width;
    int height;
    int hor_stride;
    int ver_stride;
    int rotation;
    void *pBuf;
} RgaImage;

/*======================== RGA 格式转换 ========================*/
bool rga_convert_resize(int chnId, const RgaImage &src_img, const RgaImage &dst_img);

/*======================== DMA-BUF 导入句柄 RAII 封装 ========================*/
/**
 * @brief 在 src FD 仍然有效时把它 import 到 RGA 内核, 拿到一个稳定的 handle.
 *        librga 内部会对 dma_buf 增加引用计数, 即使原始 FD 后续被关闭, RGA
 *        仍然可以安全使用这块物理内存. handle 通过 RAII 释放.
 *
 * 用途: 解决 VPU FD 在解码回调返回后被 GStreamer 释放, 而工作线程仍持有
 *       裸 FD 时触发 "rga_mm_get_channel_external_buffer dma_buf_get fail"
 *       的生命周期问题.
 */
struct RgaImportedBuffer
{
    rga_buffer_handle_t handle = 0;
    int width = 0;    // visible width
    int height = 0;   // visible height
    int stride_w = 0; // hor stride
    int stride_h = 0; // ver stride
    int format = 0;   // RK_FORMAT_*

    RgaImportedBuffer() = default;
    RgaImportedBuffer(const RgaImportedBuffer &) = delete;
    RgaImportedBuffer &operator=(const RgaImportedBuffer &) = delete;
    ~RgaImportedBuffer();
};

/**
 * 同一业务帧的惰性图像容器。
 * - source handle 只保留 DMA-BUF 引用，不主动转换像素；
 * - model_frame()/source_frame() 各自在第一次调用时生成一份 BGR 并缓存；
 * - borrowed_data 仅服务同步解码回调，回调结束前必须 clear_borrowed_source()。
 */
class LazyVideoFrame
{
  public:
    LazyVideoFrame(int channel_id, std::shared_ptr<RgaImportedBuffer> source, int source_width, int source_height,
                   int source_stride_w, int source_stride_h, int source_format, int model_width, int model_height,
                   const void *borrowed_data = nullptr);

    const cv::Mat *model_frame();
    const cv::Mat *source_frame();
    void clear_borrowed_source();
    bool available() const;
    int source_width() const { return source_width_; }
    int source_height() const { return source_height_; }

  private:
    bool materialize_borrowed(int dst_width, int dst_height, cv::Mat &out);

    int channel_id_ = -1;
    std::shared_ptr<RgaImportedBuffer> source_;
    int source_width_ = 0;
    int source_height_ = 0;
    int source_stride_w_ = 0;
    int source_stride_h_ = 0;
    int source_format_ = 0;
    int model_width_ = 0;
    int model_height_ = 0;
    const void *borrowed_data_ = nullptr;
    mutable std::mutex mutex_;
    cv::Mat model_bgr_;
    cv::Mat source_bgr_;
    bool model_attempted_ = false;
    bool source_attempted_ = false;
};

/**
 * @brief 在 FD 仍然有效时立即调用, 返回 shared_ptr; 失败时返回 nullptr (调用方走软件回退).
 */
std::shared_ptr<RgaImportedBuffer> rga_import_src_fd(int fd, int w, int h, int stride_w, int stride_h, int fmt);

/**
 * @brief 用已 import 的 src handle 走 RGA.
 *        src: 入队时 import 的稳定 handle.
 *        dst: 优先用 cached_dst_handle (模型初始化时一次性 import, 零 ioctl 开销);
 *             若 cached_dst_handle == 0 则退回每帧 importbuffer_fd 路径 (兜底安全).
 * @param cached_dst_handle  模型预缓存的 RGA handle (rga_buffer_handle_t), 0 = 不使用缓存
 */
bool rga_convert_resize_handle(int chnId, const RgaImportedBuffer &src, int dst_fd, int dst_w, int dst_h,
                               int dst_stride_w, int dst_stride_h, int dst_fmt, int cached_dst_handle = 0);

/** 用稳定的源 handle 按需生成 CPU BGR 图；供 Logic 截图和零拷贝推理失败兜底共用。 */
bool rga_convert_resize_handle_to_bgr(int chnId, const RgaImportedBuffer &src, int dst_w, int dst_h, cv::Mat &out);

/*======================== 格式字符串 → RK_FORMAT ========================*/
int rgaFmt(const char *strFmt);

/*======================== 显示 tile 布局 ========================*/
int tile_x(int chnId);
int tile_y(int chnId);
int tile_width(int chnId);
int tile_height(int chnId);
uint64_t calcBufMapOffset(int chnId, int bytesPerPixel);

/*======================== 显示缓冲区提交 ========================*/
void commitImgtoDispBufMap(int chnId, const void *pSrcData, int srcFmt, int srcWidth, int srcHeight, int srcHStride,
                           int srcVStride);
