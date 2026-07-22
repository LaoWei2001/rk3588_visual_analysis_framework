/**
 * gst_opt_impl.c — gstopt_sample_get_buffer() 通用实现
 *
 * 替代原 libgst_opt.a（广州易百珑/灵眸专有静态库），
 * 仅使用标准 GStreamer + GStreamer-Video API，
 * 可在任意厂商的 RK3588 板（正点原子、PINE64、Radxa 等）上编译运行。
 *
 * 依赖：
 *   gstreamer-1.0
 *   gstreamer-video-1.0
 *
 * 编译时通过 pkg-config 获取，无需任何厂商 SDK。
 */

#include "gst_opt.h"
#include <gst/video/video-info.h>
#include <gst/allocators/gstdmabuf.h>
#include <string.h>

/**
 * gstopt_sample_get_buffer - 从 GstSample 中提取帧缓冲区及帧描述信息
 *
 * @sample:     GStreamer appsink 的 GstSample
 * @pFrameDesc: 输出参数，填充宽、高、水平步长、垂直步长、像素格式字符串
 *
 * 返回值：GstSample 内部的 GstBuffer 指针（不增加引用计数，生命周期与 sample 绑定）；
 *         出错时返回 NULL。
 *
 * 实现说明：
 *   - 原始实现由灵眸专有 libgst_opt.a 提供，本文件以标准 API 等价替换。
 *   - GStreamer 的格式名称（NV12/NV21/BGR/RGB 等）与 Rockchip RGA 格式名称一致，
 *     直接透传给 imgDesc.fmt 无需转换。
 *   - 垂直步长 = UV 平面偏移 / Y 平面行步长（NV12/NV21 适用）；
 *     单平面格式（BGR/RGB）退化为 height。
 */
GstBuffer *gstopt_sample_get_buffer(GstSample *sample, FrameDesc_t *pFrameDesc)
{
    if (!sample || !pFrameDesc)
        return NULL;

    memset(pFrameDesc, 0, sizeof(*pFrameDesc));

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!buffer)
        return NULL;

    GstCaps *caps = gst_sample_get_caps(sample);
    if (!caps)
    {
        /* caps 不可用时尝试从 buffer 的 meta 拿，最差情况返回裸 buffer */
        return buffer;
    }

    GstVideoInfo vinfo;
    if (!gst_video_info_from_caps(&vinfo, caps))
    {
        /* 非 video/x-raw caps（不应出现），直接返回 buffer */
        return buffer;
    }

    pFrameDesc->width  = (gint)GST_VIDEO_INFO_WIDTH(&vinfo);
    pFrameDesc->height = (gint)GST_VIDEO_INFO_HEIGHT(&vinfo);

    /*
     * 优先使用 GstVideoMeta 获取实际的水平/垂直步长。
     *
     * 为什么不能只用 gst_video_info_from_caps：
     *   caps 中的 GstVideoInfo 仅记录逻辑尺寸（如 1280×720），计算出的 UV 偏移
     *   也是逻辑值（1280×720 = 921 600）。但 Rockchip mppvideodec 硬件解码器
     *   分配 DMA buffer 时，垂直方向会按 16 行对齐（VPU 硬件要求），720p 视频
     *   实际分配高度为 736，UV 平面真实偏移是 1280×736 = 942 080。
     *
     *   若用逻辑 verStride=720 送给 RGA/OpenCV，UV 起始地址会提前 16 行，
     *   这段"垫高"区域全是零（Cb=0, Cr=0）。以 BT.601 公式换算：
     *     Y=100, Cb=0, Cr=0  →  R≈0, G≈252, B≈0  ← 亮绿色！
     *   表现就是视频顶部出现约 32 行绿色条纹。
     *
     *   GstVideoMeta 由解码器在 buffer 分配时填写，包含硬件实际步长与平面偏移，
     *   是获取正确 verStride 的唯一可靠来源。
     */
    GstVideoMeta *vmeta = gst_buffer_get_video_meta(buffer);
    if (vmeta && vmeta->n_planes >= 1 && vmeta->stride[0] > 0)
    {
        /* GstVideoMeta 路径：使用硬件实际分配的步长 */
        pFrameDesc->horStride = (gint)vmeta->stride[0];
        if (vmeta->n_planes >= 2)
        {
            pFrameDesc->verStride = (gint)(vmeta->offset[1] / (gsize)vmeta->stride[0]);
        }
        else
        {
            pFrameDesc->verStride = pFrameDesc->height;
        }
    }
    else
    {
        /*
         * 回退路径：caps 逻辑尺寸（mppvideodec 经 decodebin 时理论上总会附带
         * GstVideoMeta，此分支仅作保险兜底，或用于软件解码器场景）。
         */
        pFrameDesc->horStride = (gint)GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 0);
        if (GST_VIDEO_INFO_N_PLANES(&vinfo) >= 2 && pFrameDesc->horStride > 0)
        {
            pFrameDesc->verStride = (gint)(
                GST_VIDEO_INFO_PLANE_OFFSET(&vinfo, 1) / (gsize)pFrameDesc->horStride);
        }
        else
        {
            pFrameDesc->verStride = pFrameDesc->height;
        }
    }

    /* 格式名称字符串（GStreamer 与 RGA 命名一致：NV12/NV21/BGR/RGB...） */
    const gchar *fmt_str = gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&vinfo));
    if (fmt_str)
        g_strlcpy(pFrameDesc->strFmt, fmt_str, sizeof(pFrameDesc->strFmt));

    /* 尝试提取 DMA-BUF FD (RK3588 VPU 解码后通常是 dmabuf) */
    pFrameDesc->fd = -1;
    if (gst_buffer_n_memory(buffer) > 0)
    {
        GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
        if (gst_is_dmabuf_memory(mem))
        {
            pFrameDesc->fd = gst_dmabuf_memory_get_fd(mem);
        }
    }

    return buffer;
}
