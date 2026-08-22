#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C"
{
#endif

    /** 每通道显示工作线程，由 main 通过 pthread_create 启动。 */
    void *display_worker_thread(void *arg);

#ifdef __cplusplus
}
#endif

int tile_x(int channel_id);
int tile_y(int channel_id);
int tile_width(int channel_id);
int tile_height(int channel_id);
uint64_t display_buffer_offset(int channel_id, int bytes_per_pixel);

void display_commit_frame(int channel_id, const void *source_data, int source_format, int source_width,
                          int source_height, int source_horizontal_stride, int source_vertical_stride);
