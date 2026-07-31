/**
 * @file config_init.cpp
 * @brief 配置字段注册
 */
#include "config.h"
#include "config_registry.h"

void init_config_fields(AppConfig &cfg)
{
    (void)cfg;

    // 全局配置
    REG_G("enable_display", BOOL, enable_display);
    REG_G("disp_width", INT, disp_width);
    REG_G("disp_height", INT, disp_height);
    REG_G("tile_cols", INT, tile_cols);
    REG_G("tile_rows", INT, tile_rows);
    REG_G("performance_display", BOOL, performance_display);
    REG_G("debug_display", BOOL, debug_display);
    REG_G("enable_pause_key", BOOL, enable_pause_key);

    // RTSP 推流
    REG_G("enable_rtsp", BOOL, enable_rtsp);
    REG_G("rtsp_port", INT, rtsp_port);
    REG_G("rtsp_path", STRING, rtsp_path);
    REG_G("rtsp_fps", INT, rtsp_fps);
    REG_G("rtsp_bitrate", INT, rtsp_bitrate);
    REG_G("rtsp_codec", STRING, rtsp_codec);
    REG_G("rtsp_encoder", STRING, rtsp_encoder);
    REG_G("channel_threads", INT, channel_threads);
    REG_G("max_fps", INT, max_fps);
    REG_G("local_default_fps", INT, local_default_fps);
    REG_G("queue_size", INT, queue_size);
    REG_G("tracker_enable", INT, tracker_enable);
    REG_G("tracker_iou_thresh", FLOAT, tracker_iou_thresh);
    REG_G("tracker_max_miss", INT, tracker_max_miss);
    REG_G("tracker_min_hits", INT, tracker_min_hits);

    // 通道配置
    REG_C("id", INT, id);
    REG_C("enable", BOOL, enable);
    REG_C("infer_enable", BOOL, infer_enable);
    REG_C("swap_rb", BOOL, swap_rb);
    REG_C("logic", STRING, logic);
    REG_C("logic_parameters", JSON, logic_parameters_json);
    REG_C("threads", INT, threads);
    REG_C("playback_fps", INT, playback_fps);
    REG_C("max_fps", INT, max_fps);
    REG_C("tracker_enable", INT, tracker_enable);
    REG_C("tracker_iou_thresh", FLOAT, tracker_iou_thresh);
    REG_C("tracker_max_miss", INT, tracker_max_miss);
    REG_C("tracker_min_hits", INT, tracker_min_hits);

    REG_C("report_policy", JSON, report_policy_json);
    REG_C("report_parameters", JSON, report_parameters_json);
}
