#pragma once

#include <cstdint>
#include <opencv2/core.hpp>

int tile_x(int channel_id);
int tile_y(int channel_id);
int tile_width(int channel_id);
int tile_height(int channel_id);
uint64_t display_buffer_offset(int channel_id, int bytes_per_pixel);

void display_commit_frame(int channel_id, const void *source_data, int source_format, int source_width,
                          int source_height, int source_horizontal_stride, int source_vertical_stride);

/* 只刷新最近一帧的叠加信息，不计入预览合成 FPS。 */
void display_refresh_channel_overlays(int channel_id);

/* RGA conversion into caller-owned, 16-pixel-stride BGR staging; no overlays or framebuffer writes. */
bool display_prepare_source_frame(int channel_id, const void *data, int format, int width, int height,
                                  int horizontal_stride, int vertical_stride, int output_width, int output_height,
                                  cv::Mat &staging);
