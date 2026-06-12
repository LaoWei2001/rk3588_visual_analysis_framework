/**
 * @file frame_pipeline.cpp
 * @brief 帧处理管线 — 实现已拆分到独立文件
 *
 * 此文件为空占位，保留以便将来放置跨两个子模块的公共辅助。
 *
 * 拆分后的文件职责:
 *   rga_convert.cpp   — RGA 硬件转换 + YOLO 输入帧准备
 *                        (rga_convert_resize / rga_import_src_fd /
 *                         rga_convert_resize_handle / convertToYoloInput / rgaFmt)
 *
 *   display_render.cpp — 显示 tile 布局 + framebuffer 提交
 *                        (tile_x / tile_y / tile_width / tile_height /
 *                         calcBufMapOffset / commitImgtoDispBufMap)
 *
 * 公有接口声明: frame_pipeline.h（未变动，外部模块照常引用）
 *
 * ⚠ RGA 硬性约束（在 rga_convert.cpp 中）:
 *   opt.core = IM_SCHEDULER_RGA3_CORE0 | IM_SCHEDULER_RGA3_CORE1
 *   使用 RGA2 或第三核心会硬崩溃，只能断电恢复。
 */
