#pragma once

#include <gst/gst.h>
#include <memory>

/* 保留 GstBuffer 而不只是 DMA-BUF：防止解码池在异步消费者完成前重用像素。 */
inline std::shared_ptr<void> retain_frame_source_buffer(void *buffer)
{
    if (!buffer)
        return {};
    return std::shared_ptr<void>(gst_buffer_ref(static_cast<GstBuffer *>(buffer)),
                                 [](void *value) { gst_buffer_unref(static_cast<GstBuffer *>(value)); });
}
