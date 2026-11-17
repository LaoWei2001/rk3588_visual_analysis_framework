#pragma once
#include "pipeline/frame_transform.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <gst/gst.h>
#include <linux/dma-buf.h>
#include <memory>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <vector>

namespace rtsp_detail
{
inline bool dma_buf_sync_cpu(int fd, bool start)
{
    struct dma_buf_sync sync_request
    {
    };
    sync_request.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_WRITE;
    int ret;
    do
    {
        ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync_request);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0 && errno != ENOTTY)
    {
        fprintf(stderr, "[RTSP] DMA_BUF_IOCTL_SYNC failed: fd=%d errno=%d (%s)\n", fd, errno, strerror(errno));
        return false;
    }
    return true;
}

struct DmaFrameSlot
{
    int fd{-1};
    void *mapped{MAP_FAILED};
    size_t size{0};
    std::shared_ptr<RgaImportedBuffer> rga;
    std::atomic<bool> in_use{false};
    ~DmaFrameSlot();
};

struct DmaFramePool
{
    std::shared_ptr<DmaFrameSlot> rgb_source;
    std::vector<std::shared_ptr<DmaFrameSlot>> nv12_outputs;
    int src_w{0};
    int src_h{0};
    int out_stride_w{0};
    int out_stride_h{0};

    static std::shared_ptr<DmaFramePool> create(int source_width, int source_height, int output_width,
                                                int output_height);
    std::shared_ptr<DmaFrameSlot> acquire_output()
    {
        for (const auto &slot : nv12_outputs)
        {
            bool expected = false;
            if (slot->in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return slot;
        }
        return nullptr;
    }
    void release_output(const std::shared_ptr<DmaFrameSlot> &slot)
    {
        if (slot)
            slot->in_use.store(false, std::memory_order_release);
    }
    bool begin_rgb_write()
    {
        return rgb_source && dma_buf_sync_cpu(rgb_source->fd, true);
    }
    bool end_rgb_write()
    {
        return rgb_source && dma_buf_sync_cpu(rgb_source->fd, false);
    }
    bool convert_to_nv12(const std::shared_ptr<DmaFrameSlot> &output)
    {
        if (!rgb_source || !rgb_source->rga || !output || !output->rga)
            return false;
        return rga_convert_resize_handle(-1, *rgb_source->rga, output->fd, src_w, src_h, out_stride_w, out_stride_h,
                                         RK_FORMAT_YCbCr_420_SP, static_cast<int>(output->rga->handle));
    }
};

GstBuffer *make_dmabuf_video_buffer(GstAllocator *allocator, const std::shared_ptr<DmaFrameSlot> &slot, int width,
                                    int height);
} // namespace rtsp_detail
