#include "rtsp_dma.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rtsp_detail
{
static constexpr size_t RTSP_DMA_OUTPUT_SLOTS = 6;

DmaFrameSlot::~DmaFrameSlot()
{
    rga.reset();
    if (mapped != MAP_FAILED)
        munmap(mapped, size);
    if (fd >= 0)
        close(fd);
}

static int open_rtsp_dma_heap()
{
    static const char *const heap_paths[] = {"/dev/dma_heap/rk-dma-heap-cma", "/dev/dma_heap/linux,cma",
                                             "/dev/dma_heap/cma", "/dev/dma_heap/system-uncached",
                                             "/dev/dma_heap/system"};
    for (const char *path : heap_paths)
    {
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0)
        {
            printf("[RTSP] DMA heap selected: %s\n", path);
            return fd;
        }
    }
    return -1;
}

static std::shared_ptr<DmaFrameSlot> allocate_dma_slot(int heap_fd, size_t size, int width, int height, int stride_w,
                                                       int stride_h, int format)
{
    struct dma_heap_allocation_data allocation
    {
    };
    allocation.len = size;
    allocation.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation) < 0)
    {
        fprintf(stderr, "[RTSP] DMA_HEAP_IOCTL_ALLOC failed: size=%zu errno=%d (%s)\n", size, errno, strerror(errno));
        return nullptr;
    }

    const int dma_fd = static_cast<int>(allocation.fd);
    void *mapped = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, dma_fd, 0);
    if (mapped == MAP_FAILED)
    {
        fprintf(stderr, "[RTSP] mmap DMA-BUF failed: size=%zu errno=%d (%s)\n", size, errno, strerror(errno));
        close(dma_fd);
        return nullptr;
    }

    auto imported = rga_import_src_fd(dma_fd, width, height, stride_w, stride_h, format);
    if (!imported)
    {
        fprintf(stderr, "[RTSP] RGA cannot import allocated DMA-BUF: fd=%d format=%d\n", dma_fd, format);
        munmap(mapped, size);
        close(dma_fd);
        return nullptr;
    }

    auto slot = std::make_shared<DmaFrameSlot>();
    slot->fd = dma_fd;
    slot->mapped = mapped;
    slot->size = size;
    slot->rga = std::move(imported);
    return slot;
}

std::shared_ptr<DmaFramePool> DmaFramePool::create(int source_width, int source_height, int output_width,
                                                   int output_height)
{
    const int heap_fd = open_rtsp_dma_heap();
    if (heap_fd < 0)
    {
        fprintf(stderr, "[RTSP] no usable DMA heap found; hardware encoder DMA path unavailable\n");
        return nullptr;
    }

    auto pool = std::make_shared<DmaFramePool>();
    pool->src_w = source_width;
    pool->src_h = source_height;
    pool->out_stride_w = output_width;
    pool->out_stride_h = output_height;
    pool->rgb_source = allocate_dma_slot(heap_fd, static_cast<size_t>(source_width) * source_height * 3, source_width,
                                         source_height, source_width, source_height, RK_FORMAT_RGB_888);
    const size_t nv12_size = static_cast<size_t>(output_width) * output_height * 3 / 2;
    for (size_t index = 0; pool->rgb_source && index < RTSP_DMA_OUTPUT_SLOTS; ++index)
    {
        auto output = allocate_dma_slot(heap_fd, nv12_size, output_width, output_height, output_width, output_height,
                                        RK_FORMAT_YCbCr_420_SP);
        if (!output)
            break;
        if (!dma_buf_sync_cpu(output->fd, true))
            break;
        memset(output->mapped, 0, static_cast<size_t>(output_width) * output_height);
        memset(static_cast<unsigned char *>(output->mapped) + static_cast<size_t>(output_width) * output_height, 128,
               nv12_size - static_cast<size_t>(output_width) * output_height);
        if (!dma_buf_sync_cpu(output->fd, false))
            break;
        pool->nv12_outputs.push_back(std::move(output));
    }
    close(heap_fd);

    if (!pool->rgb_source || pool->nv12_outputs.size() != RTSP_DMA_OUTPUT_SLOTS)
    {
        fprintf(stderr, "[RTSP] DMA frame pool initialization failed (%zu/%zu output slots)\n",
                pool->nv12_outputs.size(), RTSP_DMA_OUTPUT_SLOTS);
        return nullptr;
    }
    printf("[RTSP] DMA frame pool ready: RGB source + %zu NV12 output slots\n", pool->nv12_outputs.size());
    return pool;
}

struct DmaSlotLease
{
    std::shared_ptr<DmaFrameSlot> slot;
};

static void release_dma_slot_when_memory_dies(gpointer user_data, GstMiniObject *object)
{
    (void)object;
    std::unique_ptr<DmaSlotLease> lease(static_cast<DmaSlotLease *>(user_data));
    lease->slot->in_use.store(false, std::memory_order_release);
}

GstBuffer *make_dmabuf_video_buffer(GstAllocator *allocator, const std::shared_ptr<DmaFrameSlot> &slot, int width,
                                    int height)
{
    if (!allocator || !slot)
        return nullptr;
    const int exported_fd = fcntl(slot->fd, F_DUPFD_CLOEXEC, 0);
    if (exported_fd < 0)
        return nullptr;

    GstMemory *memory = gst_dmabuf_allocator_alloc(allocator, exported_fd, slot->size);
    if (!memory)
    {
        close(exported_fd);
        return nullptr;
    }

    GstBuffer *buffer = gst_buffer_new();
    auto *lease = new DmaSlotLease{slot};
    gst_mini_object_weak_ref(GST_MINI_OBJECT(memory), release_dma_slot_when_memory_dies, lease);
    if (!buffer)
    {
        gst_memory_unref(memory);
        return nullptr;
    }
    gst_buffer_append_memory(buffer, memory);

    gsize offsets[GST_VIDEO_MAX_PLANES] = {0};
    gint strides[GST_VIDEO_MAX_PLANES] = {0};
    offsets[1] = static_cast<gsize>(width) * height;
    strides[0] = width;
    strides[1] = width;
    gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12, width, height, 2, offsets,
                                   strides);

    return buffer;
}
} // namespace rtsp_detail
